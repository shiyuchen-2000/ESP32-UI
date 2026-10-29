#include "nav_proto.h"
#include "nav_map.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nav_proto";

static nav_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_rx_log_budget = 12;
/* Last time any BLE message (text or bulk) arrived. The UI uses it to tell a
 * live link from one where the app stopped sending but never disconnected -
 * without it the last navigation frame stayed on screen forever. */
static volatile uint32_t s_last_rx_ms;

/* Bulk transfer bookkeeping ("M" packets). The first stage only ACKs complete
 * transfers so the phone stops resending; map payloads are added later. */
#define BULK_TYPES   5
#define BULK_MAX_PARTS 4096
typedef struct {
    int      id;
    int      parts;
    int      got;
    uint32_t total;
    uint8_t *buf;          /* PSRAM, owned until complete */
    uint8_t  mask[BULK_MAX_PARTS / 8];
} bulk_rx_t;
static bulk_rx_t s_bulk[BULK_TYPES];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void nav_proto_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_state, 0, sizeof(s_state));
    s_state.theme = 1;      /* the app's default: Vice City */
    s_state.map_on = true;
    s_state.trail = 1;
    taskEXIT_CRITICAL(&s_lock);
    memset(s_bulk, 0, sizeof(s_bulk));
    nav_map_init();
}

void nav_proto_set_link(bool connected, bool subscribed)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.connected = connected;
    s_state.subscribed = subscribed;
    if (!connected) {
        s_state.active = false;
        s_state.turn = NAV_TURN_NONE;
        s_state.distance_m = 0;
        s_state.speed_kmh = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "link connected=%d subscribed=%d", connected, subscribed);
}

void nav_proto_get(nav_state_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    taskEXIT_CRITICAL(&s_lock);
}

static void handle_bulk(const uint8_t *d, uint16_t n)
{
    if (n < 15) {
        return;
    }
    int type = d[1];
    int id = d[2];
    int parts = d[3] | (d[4] << 8);
    int seq = d[5] | (d[6] << 8);
    uint32_t total = rd32(d + 7);
    uint32_t off = rd32(d + 11);
    if (type < 1 || type >= BULK_TYPES || total == 0 || parts <= 0 ||
        parts > BULK_MAX_PARTS || seq >= parts || off >= total) {
        return;
    }

    bulk_rx_t *b = &s_bulk[type];
    if (id != b->id || parts != b->parts || total != b->total) {
        if (b->buf) {
            free(b->buf);
            b->buf = NULL;
        }
        b->id = id;
        b->parts = parts;
        b->total = total;
        b->got = 0;
        memset(b->mask, 0, sizeof(b->mask));
        b->buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b->buf) {
            ESP_LOGE(TAG, "no PSRAM for bulk type=%d total=%u", type, (unsigned)total);
        }
    }
    if (b->got >= b->parts) {
        /* duplicate after completion: ACK again so the phone stops retrying */
        char ack[24];
        snprintf(ack, sizeof(ack), "K,%d,%d", type, id);
        nav_ble_notify(ack);
        return;
    }
    if (b->buf) {
        uint32_t payload = n - 15;
        if (off + payload > total) {
            payload = total - off;
        }
        memcpy(b->buf + off, d + 15, payload);
    }
    if (!(b->mask[seq >> 3] & (1 << (seq & 7)))) {
        b->mask[seq >> 3] |= (uint8_t)(1 << (seq & 7));
        b->got++;
    }
    if (b->got >= b->parts) {
        if (b->buf) {
            nav_map_set_blob(type, b->buf, total);
            b->buf = NULL;
        }
        char ack[24];
        snprintf(ack, sizeof(ack), "K,%d,%d", type, id);
        nav_ble_notify(ack);
        ESP_LOGI(TAG, "bulk type=%d id=%d complete (%u bytes), ACK", type, id, (unsigned)total);
    }
}

static void handle_text(const char *p)
{
    if (s_rx_log_budget > 0) {
        ESP_LOGI(TAG, "rx: %s", p);
        s_rx_log_budget--;
    }

    if (p[0] == 'P' && (p[1] == '\0' || p[1] == ',')) {
        return;   /* heartbeat */
    }

    /* Turn / distance: "<dir>[,<metres>[,<angle>]]" for EVERY direction.
     * The app always appends the distance (pushNav sends "SL,480", "U,300",
     * ...), but this parser used to accept SL/SR/U only as bare strings and
     * dropped the version with a distance. After a turn whose next maneuver
     * was one of those, the HUD kept the previous instruction - normally
     * "0 m" - until the next L/R/S arrived. */
    if (p[0] != '#') {
        const char *c1 = strchr(p, ',');
        int dlen = c1 ? (int)(c1 - p) : (int)strlen(p);
        nav_turn_t turn = NAV_TURN_NONE;
        if (dlen == 1) {
            switch (p[0]) {
            case 'R': turn = NAV_TURN_RIGHT; break;
            case 'L': turn = NAV_TURN_LEFT; break;
            case 'S': turn = NAV_TURN_STRAIGHT; break;
            case 'U': turn = NAV_TURN_UTURN; break;
            case 'A': turn = NAV_TURN_ARRIVE; break;
            default: break;
            }
        } else if (dlen == 2 && p[0] == 'S') {
            if (p[1] == 'L') {
                turn = NAV_TURN_SLIGHT_LEFT;
            } else if (p[1] == 'R') {
                turn = NAV_TURN_SLIGHT_RIGHT;
            }
        }
        if (turn != NAV_TURN_NONE) {
            int dist = 0, angle = 0;
            if (c1) {
                dist = atoi(c1 + 1);
                const char *c2 = strchr(c1 + 1, ',');
                if (c2) {
                    angle = atoi(c2 + 1);
                }
            }
            taskENTER_CRITICAL(&s_lock);
            s_state.active = true;
            s_state.turn = turn;
            s_state.turn_angle = angle;
            if (turn == NAV_TURN_ARRIVE) {
                s_state.distance_m = 0;
            } else if (c1) {
                s_state.distance_m = dist;
            }
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "turn -> %d dist=%d angle=%d", (int)turn, dist, angle);
            return;
        }
    }
    if (strcmp(p, "C") == 0) {
        taskENTER_CRITICAL(&s_lock);
        s_state.active = false;
        s_state.turn = NAV_TURN_NONE;
        s_state.distance_m = 0;
        s_state.speed_kmh = 0;
        s_state.has_trip = false;
        s_state.remain_m = 0;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    if (strncmp(p, "#v,", 3) == 0) {
        int v = atoi(p + 3);
        taskENTER_CRITICAL(&s_lock);
        s_state.speed_kmh = v;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#e,", 3) == 0) {
        int remain = 0, eta = 0, lights = 0;
        int n = sscanf(p + 3, "%d,%d,%d", &remain, &eta, &lights);
        taskENTER_CRITICAL(&s_lock);
        if (n >= 1) {
            s_state.has_trip = true;
            s_state.remain_m = remain;
            s_state.eta_hhmm = eta;
            s_state.lights = lights;
        } else {
            s_state.has_trip = false;
        }
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strcmp(p, "#e") == 0) {
        taskENTER_CRITICAL(&s_lock);
        s_state.has_trip = false;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#p,", 3) == 0) {
        int frame = 0, xdm = 0, ydm = 0, h = 0, v = 0;
        if (sscanf(p + 3, "%d,%d,%d,%d,%d", &frame, &xdm, &ydm, &h, &v) >= 4) {
            taskENTER_CRITICAL(&s_lock);
            s_state.pos_frame = frame;
            s_state.pos_x_dm = xdm;
            s_state.pos_y_dm = ydm;
            s_state.heading = h;
            if (v >= 0) {
                s_state.speed_kmh = v;
            }
            taskEXIT_CRITICAL(&s_lock);
        }
        return;
    }
    if (strncmp(p, "#t,", 3) == 0) {
        int t = atoi(p + 3);
        taskENTER_CRITICAL(&s_lock);
        s_state.theme = t;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "map style -> %d", t);
        return;
    }
    if (strncmp(p, "#n,", 3) == 0) {
        bool on = atoi(p + 3) != 0;
        taskENTER_CRITICAL(&s_lock);
        s_state.night = on;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#m,", 3) == 0) {
        bool on = atoi(p + 3) != 0;
        taskENTER_CRITICAL(&s_lock);
        s_state.map_on = on;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#r,", 3) == 0) {
        int trail = atoi(p + 3);
        taskENTER_CRITICAL(&s_lock);
        s_state.trail = trail;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#Z,", 3) == 0) {
        int have = 0, play = 0, actions = 0;
        if (sscanf(p + 3, "%d,%d,%d", &have, &play, &actions) >= 2) {
            taskENTER_CRITICAL(&s_lock);
            s_state.music_have = have != 0;
            s_state.music_playing = play != 0;
            s_state.music_actions = actions;
            taskEXIT_CRITICAL(&s_lock);
        }
        return;
    }
    if (strncmp(p, "#ZACK,", 6) == 0) {
        taskENTER_CRITICAL(&s_lock);
        s_state.music_ack = atoi(p + 6);
        s_state.music_ack_seq++;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strncmp(p, "#GB", 3) == 0) {
        /* No building capability: the map is shown as plain filled blocks, so
         * the phone should send the base raster (it flattens building pixels
         * back to ground/park via RasterPacket.forLegacy). This also keeps the
         * bulk smaller and the first map appearing sooner. */
        return;
    }

    /* Unknown text (or a future "#..." command): ignore. */
}

uint32_t nav_proto_last_rx_ms(void)
{
    return s_last_rx_ms;
}

void nav_proto_on_rx(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return;
    }
    s_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (data[0] == 'M') {
        handle_bulk(data, len);
        return;
    }
    if (data[0] == 'B') {
        return;   /* image slots (weather/music/call) are not used by nav v1 */
    }
    if (len >= 256) {
        return;   /* text messages are short; anything else is not ours */
    }
    char buf[256];
    memcpy(buf, data, len);
    buf[len] = '\0';
    handle_text(buf);
}
