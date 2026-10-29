/*
 * WiFi manager with SoftAP provisioning (no device-side scan - the scan path
 * was unstable on this board, see project memory). When no network is saved
 * the device runs an open hotspot "ESP_Setup_XXXX" and a tiny HTTP server on
 * 192.168.4.1:80. The phone connects, opens the page, types the WiFi name and
 * password, and the device saves them and joins the network in STA mode.
 *
 * All esp_wifi event callbacks run on the default event loop task and only
 * touch shared state guarded by a mutex; the UI task reads that state through
 * the getters and polls wifi_mgr_consume_changed().
 */
#include "wifi_mgr.h"
#include "rtc_pcf85063.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS              "wifi"
#define NVS_SSID            "ssid"
#define NVS_PASS            "pass"
#define NVS_ENABLED         "en"    /* user on/off switch (default 1) */

static bool s_enabled = true;

#define HTTPD_PORT          80
#define HTTPD_STACK         (6 * 1024)
#define MAX_CONNECT_RETRIES 3
#define RETRY_DELAY_MS      3000

static SemaphoreHandle_t s_lock;
static bool s_changed = false;
static bool s_ntp_synced = false;
static wifi_mgr_state_t s_state = WIFI_MGR_DISABLED;
static char s_current_ssid[33];
static char s_saved_pass[65];
static char s_ap_name[32];
static int  s_retries = 0;

static bool s_httpd_stop = false;
static TaskHandle_t s_httpd_task_h = NULL;
static esp_timer_handle_t s_retry_timer = NULL;
static bool s_stopping = false;   /* intentionally switching mode: ignore the
                                   * disconnect event that esp_wifi_stop emits */
static bool s_sntp_started = false;

/* ------------------------------------------------------------------ */
/* shared-state helpers                                               */
/* ------------------------------------------------------------------ */
static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void set_state(wifi_mgr_state_t st)
{
    if (s_state != st) {
        s_state = st;
        s_changed = true;
    }
}

static void notify_changed(void)
{
    s_changed = true;
}

/* ------------------------------------------------------------------ */
/* NVS credentials                                                    */
/* ------------------------------------------------------------------ */
static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_get_str(h, NVS_SSID, ssid, &ssid_len) == ESP_OK);
    if (ok) {
        size_t plen = pass_len;
        if (nvs_get_str(h, NVS_PASS, pass, &plen) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return ok;
}

static void erase_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, NVS_SSID);
    nvs_erase_key(h, NVS_PASS);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 1;   /* first boot: WiFi on, same as the old behaviour */
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_ENABLED, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void save_enabled(bool en)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_ENABLED, en ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* NTP                                                                */
/* ------------------------------------------------------------------ */
static void ntp_sync_cb(struct timeval *tv)
{
    lock();
    s_ntp_synced = true;
    s_changed = true;
    unlock();
    ESP_LOGI(TAG, "NTP time synced");

    /* Persist the authoritative time so the watch keeps it across power
     * cycles with WiFi switched off. */
    rtc_pcf85063_set_time(tv ? tv->tv_sec : time(NULL));
}

static void ntp_start(void)
{
    if (s_sntp_started) {
        return;   /* already running */
    }
    s_sntp_started = true;
    setenv("TZ", CONFIG_ESP_UI_TIMEZONE, 1);
    tzset();
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ntp.wait_for_sync = false;
    ntp.sync_cb = ntp_sync_cb;
    esp_netif_sntp_init(&ntp);
}

/* ------------------------------------------------------------------ */
/* events                                                             */
/* ------------------------------------------------------------------ */
static void retry_connect(void)
{
    lock();
    set_state(WIFI_MGR_CONNECTING);
    unlock();
    esp_wifi_connect();
}

static void retry_timer_cb(void *arg)
{
    retry_connect();
}

static void schedule_retry(void)
{
    if (!s_retry_timer) {
        esp_timer_create(&(esp_timer_create_args_t){
            .callback = retry_timer_cb, .name = "wifi_retry",
        }, &s_retry_timer);
    }
    esp_timer_start_once(s_retry_timer, RETRY_DELAY_MS * 1000);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            lock();
            s_stopping = false;
            set_state(WIFI_MGR_CONNECTING);
            unlock();
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            break;   /* wait for IP */
        case WIFI_EVENT_STA_DISCONNECTED:
            {
                wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
                lock();
                bool stopping = s_stopping;
                unlock();
                if (stopping) {
                    break;   /* caused by our own esp_wifi_stop during a mode switch */
                }
                ESP_LOGW(TAG, "disconnected, reason=%d", d->reason);
                lock();
                s_retries++;
                int r = s_retries;
                unlock();
                if (r >= MAX_CONNECT_RETRIES) {
                    ESP_LOGW(TAG, "giving up after %d tries; back to provisioning", r);
                    wifi_mgr_start_provisioning();
                } else {
                    lock();
                    set_state(WIFI_MGR_FAILED);
                    unlock();
                    schedule_retry();
                }
            }
            break;
        case WIFI_EVENT_AP_START:
            lock();
            s_stopping = false;
            set_state(WIFI_MGR_PROVISIONING);
            unlock();
            ESP_LOGI(TAG, "softAP %s active", s_ap_name);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            notify_changed();   /* phone joined the hotspot */
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        lock();
        s_retries = 0;
        set_state(WIFI_MGR_CONNECTED);
        unlock();
        ntp_start();
    }
}

/* ------------------------------------------------------------------ */
/* provisioning HTTP server                                           */
/* ------------------------------------------------------------------ */
static const char PROV_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>WiFi Setup</title><style>"
    "body{font-family:sans-serif;max-width:400px;margin:30px auto;padding:0 16px;color:#222}"
    "h1{font-size:22px}input{width:100%;padding:12px;margin:8px 0;font-size:16px;box-sizing:border-box;border:1px solid #ccc;border-radius:6px}"
    "button{width:100%;padding:14px;font-size:18px;background:#00b894;color:#fff;border:0;border-radius:6px;margin-top:12px}"
    "</style></head><body><h1>Watch WiFi Setup</h1>"
    "<p>Enter your home WiFi details:</p>"
    "<form method=\"post\" action=\"/save\">"
    "<input name=\"ssid\" placeholder=\"WiFi name (SSID)\" required autofocus>"
    "<input name=\"pass\" type=\"password\" placeholder=\"Password\">"
    "<button type=\"submit\">Connect</button></form></body></html>";

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Find "key=" in the form body and copy the URL-decoded value to out. */
static void parse_field(const char *body, const char *key, char *out, size_t out_size)
{
    const char *p = strstr(body, key);
    if (!p) {
        out[0] = '\0';
        return;
    }
    p += strlen(key);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) {
        len = out_size - 1;
    }
    char tmp[128];
    if (len > sizeof(tmp) - 1) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    url_decode(out, tmp, out_size);
}

static void handle_client(int sock)
{
    char buf[2048];
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    if (strncmp(buf, "POST", 4) == 0) {
        char *body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            char ssid[33] = { 0 }, pass[65] = { 0 };
            parse_field(body, "ssid=", ssid, sizeof(ssid));
            parse_field(body, "pass=", pass, sizeof(pass));
            const char *resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                "<html><body style=\"font-family:sans-serif;margin:40px;text-align:center\">"
                "<h2>Saved - connecting to WiFi...</h2>"
                "<p>You can close this page.</p></body></html>";
            send(sock, resp, strlen(resp), 0);
            ESP_LOGI(TAG, "got creds: ssid=%s pass_len=%d", ssid, (int)strlen(pass));
            wifi_mgr_connect(ssid, pass);
        }
    } else {
        char resp[1600];
        int len = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(PROV_HTML), PROV_HTML);
        if (len > 0) {
            send(sock, resp, len, 0);
        }
    }
}

static void s_httpd_running_end(void)
{
    s_httpd_stop = true;
}

static void httpd_task(void *arg)
{
    int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock < 0) {
        s_httpd_running_end();
        s_httpd_task_h = NULL;
        vTaskDelete(NULL);
    }
    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTPD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lsock, 2) != 0) {
        ESP_LOGE(TAG, "HTTP bind/listen failed");
        close(lsock);
        s_httpd_running_end();
        s_httpd_task_h = NULL;
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "HTTP server listening on :%d", HTTPD_PORT);

    while (!s_httpd_stop) {
        /* poll with a timeout so httpd_stop() (which just sets the flag) can
         * wake the task; avoids racing a shared listen-socket handle */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lsock, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int r = select(lsock + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            break;
        }
        if (r == 0 || !FD_ISSET(lsock, &rfds)) {
            continue;
        }
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cs = accept(lsock, (struct sockaddr *)&cli, &clen);
        if (cs < 0) {
            break;
        }
        handle_client(cs);
        close(cs);
    }
    close(lsock);
    s_httpd_running_end();
    s_httpd_task_h = NULL;
    vTaskDelete(NULL);   /* FreeRTOS tasks must not return from their function */
}

static void httpd_stop(void)
{
    s_httpd_stop = true;   /* the task's select() wakes within 500 ms and exits */
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
void wifi_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_name, sizeof(s_ap_name), "ESP_Setup_%02X%02X%02X", mac[3], mac[4], mac[5]);

    s_enabled = load_enabled();
    if (!s_enabled) {
        /* Driver is initialised (cheap to re-start later) but the radio stays
         * off. The RTC provides time while WiFi is disabled. */
        set_state(WIFI_MGR_DISABLED);
        ESP_LOGI(TAG, "WiFi disabled by setting; radio off");
        return;
    }

    char ssid[33], pass[65];
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        lock();
        strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
        s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
        strncpy(s_saved_pass, pass, sizeof(s_saved_pass) - 1);
        s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';
        s_retries = 0;
        unlock();
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        if (pass[0]) {
            strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        }
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START -> connect */
        ESP_LOGI(TAG, "STA auto-connecting to %s", ssid);
    } else {
        wifi_mgr_start_provisioning();
        ESP_LOGI(TAG, "no saved WiFi; softAP %s + http://192.168.4.1", s_ap_name);
    }
}

bool wifi_mgr_has_saved_creds(void)
{
    char ssid[33], pass[65];
    return load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0];
}

const char *wifi_mgr_prov_ap_name(void)
{
    return s_ap_name;
}

/* Non-fatal wifi mode switches: a failed esp_wifi_start() (common when
 * switching AP<->STA rapidly) must NOT abort the whole system. */
static bool wifi_switch_to_ap(wifi_config_t *ap)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(150));   /* let the driver settle before restarting */
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        (ap && esp_wifi_set_config(WIFI_IF_AP, ap) != ESP_OK) ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "failed to switch to AP mode");
        return false;
    }
    return true;
}

static bool wifi_switch_to_sta(wifi_config_t *sta)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(150));   /* let the driver settle before restarting */
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        (sta && esp_wifi_set_config(WIFI_IF_STA, sta) != ESP_OK) ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "failed to switch to STA mode");
        return false;
    }
    return true;
}

void wifi_mgr_start_provisioning(void)
{
    httpd_stop();
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    lock();
    s_stopping = true;   /* ignore the disconnect esp_wifi_stop() may emit */
    s_retries = 0;
    unlock();

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* open hotspot per user choice */
    if (!wifi_switch_to_ap(&ap)) {
        lock();
        set_state(WIFI_MGR_FAILED);
        unlock();
        return;
    }

    lock();
    set_state(WIFI_MGR_PROVISIONING);
    unlock();

    /* wait for any previous httpd task to exit before binding port 80 again */
    for (int i = 0; i < 20 && s_httpd_task_h; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_httpd_stop = false;
    xTaskCreate(httpd_task, "wifi_httpd", HTTPD_STACK, NULL, 5, &s_httpd_task_h);
    ESP_LOGI(TAG, "provisioning: %s http://192.168.4.1", s_ap_name);
}

void wifi_mgr_stop_provisioning(void)
{
    char ssid[33], pass[65];
    if (!(load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0])) {
        return;   /* nothing to go back to: stay in provisioning */
    }
    httpd_stop();
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    lock();
    s_stopping = true;   /* ignore the disconnect esp_wifi_stop() may emit */
    s_retries = 0;
    unlock();

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    if (pass[0]) {
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    }
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (!wifi_switch_to_sta(&sta)) {
        return;
    }
    ESP_LOGI(TAG, "reconnecting to saved network %s", ssid);   /* STA_START connects */
}

void wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return;
    }
    const char *pass = password ? password : "";

    /* stop provisioning server + AP, switch to STA */
    httpd_stop();
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    lock();
    s_stopping = true;   /* ignore the disconnect esp_wifi_stop() may emit */
    s_retries = 0;
    unlock();

    save_creds(ssid, pass);
    lock();
    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
    strncpy(s_saved_pass, pass, sizeof(s_saved_pass) - 1);
    s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';
    s_retries = 0;
    set_state(WIFI_MGR_CONNECTING);
    unlock();

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    if (pass[0]) {
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    }
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (!wifi_switch_to_sta(&sta)) {
        lock();
        set_state(WIFI_MGR_FAILED);
        unlock();
        return;
    }
    ESP_LOGI(TAG, "connecting to %s", ssid);   /* STA_START -> connect */
}

void wifi_mgr_forget(void)
{
    erase_creds();
    esp_wifi_stop();
    lock();
    s_current_ssid[0] = '\0';
    unlock();
    wifi_mgr_start_provisioning();
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    lock();
    wifi_mgr_state_t st = s_state;
    unlock();
    return st;
}

bool wifi_mgr_is_connected(void)
{
    return wifi_mgr_get_state() == WIFI_MGR_CONNECTED;
}

bool wifi_mgr_get_current_ssid(char *out, size_t len)
{
    lock();
    bool ok = (s_current_ssid[0] != '\0');
    if (ok && len > 0) {
        strncpy(out, s_current_ssid, len - 1);
        out[len - 1] = '\0';
    }
    unlock();
    return ok;
}

bool wifi_mgr_is_ntp_synced(void)
{
    lock();
    bool s = s_ntp_synced;
    unlock();
    return s;
}

bool wifi_mgr_is_enabled(void)
{
    return s_enabled;
}

void wifi_mgr_set_enabled(bool en)
{
    if (en == s_enabled) {
        return;
    }
    s_enabled = en;
    save_enabled(en);

    if (!en) {
        httpd_stop();
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        lock();
        s_stopping = true;   /* ignore the disconnect that esp_wifi_stop emits */
        s_retries = 0;
        unlock();
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(ret));
        }
        lock();
        set_state(WIFI_MGR_DISABLED);
        unlock();
        ESP_LOGI(TAG, "WiFi disabled by user");
        return;
    }

    ESP_LOGI(TAG, "WiFi enabled by user");
    char ssid[33], pass[65];
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        lock();
        strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
        s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
        strncpy(s_saved_pass, pass, sizeof(s_saved_pass) - 1);
        s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';
        s_retries = 0;
        set_state(WIFI_MGR_CONNECTING);
        unlock();

        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        if (pass[0]) {
            strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        }
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        if (!wifi_switch_to_sta(&sta)) {
            lock();
            set_state(WIFI_MGR_FAILED);
            unlock();
        }
    } else {
        wifi_mgr_start_provisioning();
    }
}

bool wifi_mgr_consume_changed(void)
{
    lock();
    bool c = s_changed;
    s_changed = false;
    unlock();
    return c;
}
