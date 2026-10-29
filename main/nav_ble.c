#include "nav_ble.h"
#include "nav_proto.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <string.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "nav_ble";

/* Nordic UART Service (MotoNav uses it verbatim). */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_tx_val_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static bool     s_subscribed;
static bool     s_enabled = true;
static char     s_name[24];
static uint8_t  s_rx_buf[512];

#define NVS_NS "navbt"

static bool load_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "en", &v);
        nvs_close(h);
    }
    return v != 0;
}

static void save_enabled(bool en)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "en", en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void advertise(void);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf), &out_len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        nav_proto_on_rx(s_rx_buf, out_len);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                /* Unencrypted on purpose: test builds skip the 6-digit pairing
                 * flow the stock firmware uses. */
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

void nav_ble_notify(const char *text)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed || !text) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, (uint16_t)strlen(text));
    if (!om) {
        return;
    }
    if (ble_gatts_notify_custom(s_conn, s_tx_val_handle, om) != 0) {
        os_mbuf_free_chain(om);
    }
}

bool nav_ble_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool nav_ble_is_enabled(void)
{
    return s_enabled;
}

const char *nav_ble_get_name(void)
{
    return s_name;
}

void nav_ble_set_enabled(bool enabled)
{
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    save_enabled(enabled);
    if (!enabled) {
        ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, 0x13);   /* remote user terminated */
        }
        s_subscribed = false;
        nav_proto_set_link(false, false);
        ESP_LOGI(TAG, "Bluetooth disabled by user");
    } else {
        ESP_LOGI(TAG, "Bluetooth enabled by user");
        advertise();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            nav_proto_set_link(true, false);
            ESP_LOGI(TAG, "phone connected, handle=%d", s_conn);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        nav_proto_set_link(false, false);
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            nav_proto_set_link(true, s_subscribed);
            ESP_LOGI(TAG, "notify subscription=%d", s_subscribed);
            if (s_subscribed) {
                /* The app marks the link verified on the first U,1 and then
                 * resends all state; the stock firmware sends it three times. */
                nav_ble_notify("U,1");
                nav_ble_notify("U,1");
                nav_ble_notify("U,1");
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!s_enabled) {
        return;   /* user switched Bluetooth off */
    }
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan rsp rc=%d", rc);
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s", s_name);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void nav_ble_init(void)
{
    nav_proto_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
    }

    s_enabled = load_enabled();
    ESP_LOGI(TAG, "BT switch state: %s", s_enabled ? "on" : "off");

    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(s_name, sizeof(s_name), "MotoNav-%02X%02X", mac[4], mac[5]);

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    ble_att_set_preferred_mtu(517);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add svcs rc=%d", rc);
        return;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE started as %s", s_name);
}
