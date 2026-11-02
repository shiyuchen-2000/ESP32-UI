/*
 * WiFi settings UI (SoftAP provisioning): shows the hotspot name and the steps
 * for configuring the network from the phone, plus a live status readout and a
 * button to (re)start provisioning. No device-side scan and no on-device
 * keyboard. Polls wifi_mgr with an lv_timer while the page is visible.
 *
 * The parent view is 360x290 at screen y56; interactive content stays above
 * screen y292 so the bottom-swipe gesture doesn't misfire.
 */
#include "ui_wifi.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "wifi_mgr.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define POLL_PERIOD_MS 300

typedef struct {
    lv_obj_t *root;
    lv_obj_t *status;   /* big status label */
    lv_obj_t *qr;       /* QR code to join the open hotspot */
    lv_obj_t *info;     /* multi-line instructions / status */
    lv_obj_t *btn;      /* (re)start setup button */
    lv_obj_t *sw;       /* WiFi on/off switch */
    lv_timer_t *poll;
    lv_obj_t *poll_owner;   /* page whose widgets the poll timer drives */
} wifi_ui_t;

static wifi_ui_t s_wifi;
static bool s_was_visible = false;

static lv_obj_t *wl_label(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    return l;
}

static bool is_visible_chain(lv_obj_t *o)
{
    while (o) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        o = lv_obj_get_parent(o);
    }
    return true;
}

static void wifi_btn_cb(lv_event_t *e)
{
    if (wifi_mgr_get_state() == WIFI_MGR_PROVISIONING) {
        wifi_mgr_stop_provisioning();   /* back to the connected network */
    } else {
        wifi_mgr_start_provisioning();
    }
}

static void update_ui(void);

static void wifi_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    wifi_mgr_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    update_ui();
}

/* ------------------------------------------------------------------ */
/* status refresh                                                     */
/* ------------------------------------------------------------------ */
static void update_ui(void)
{
    if (!s_wifi.status) {
        return;
    }
    char buf[200];
    wifi_mgr_state_t st = wifi_mgr_get_state();

    /* keep the switch in sync when the state changes elsewhere */
    if (s_wifi.sw) {
        bool en = (st != WIFI_MGR_DISABLED);
        if (en != lv_obj_has_state(s_wifi.sw, LV_STATE_CHECKED)) {
            if (en) {
                lv_obj_add_state(s_wifi.sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_wifi.sw, LV_STATE_CHECKED);
            }
        }
    }

    /* move the info text down under the QR while provisioning, up into the
     * freed space (and clear of the button) otherwise */
    if (st == WIFI_MGR_PROVISIONING) {
        lv_obj_align(s_wifi.info, LV_ALIGN_TOP_MID, 0, 146);
    } else {
        lv_obj_align(s_wifi.info, LV_ALIGN_TOP_MID, 0, 44);
    }

    switch (st) {
    case WIFI_MGR_DISABLED:
        lv_label_set_text(s_wifi.status, "WiFi 已关闭");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(UI_COLOR_DIM), 0);
        lv_label_set_text(s_wifi.info, "打开上方开关即可连接网络。");
        lv_obj_add_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        break;

    case WIFI_MGR_PROVISIONING:
        lv_label_set_text(s_wifi.status, "WiFi 配网");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(UI_COLOR_ACCENT), 0);
        /* Two lines only: the bottom hint ("从屏幕边缘滑动返回") sits at
         * y~290 and the button must stay above it. */
        snprintf(buf, sizeof(buf),
                 "热点：%s\n"
                 "扫码打开 http://192.168.4.1",
                 wifi_mgr_prov_ap_name());
        lv_label_set_text(s_wifi.info, buf);
        lv_obj_clear_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        if (wifi_mgr_has_saved_creds()) {
            lv_obj_clear_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_wifi.btn, 0), "返回 WiFi");
        } else {
            lv_obj_add_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        }
        break;

    case WIFI_MGR_CONNECTING: {
        char cur[33];
        const char *ssid = wifi_mgr_get_current_ssid(cur, sizeof(cur)) ? cur : "";
        lv_label_set_text(s_wifi.status, "连接中...");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(UI_COLOR_DIM), 0);
        lv_obj_add_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "正在连接「%s」...\n\n可能需要几秒钟。",
                 ssid);
        lv_label_set_text(s_wifi.info, buf);
        lv_obj_add_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case WIFI_MGR_CONNECTED: {
        char cur[33];
        const char *ssid = wifi_mgr_get_current_ssid(cur, sizeof(cur)) ? cur : "";
        lv_label_set_text(s_wifi.status, "已连接");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_add_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "已连接「%s」。\n\n点击按钮可更换网络。",
                 ssid);
        lv_label_set_text(s_wifi.info, buf);
        lv_obj_clear_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_wifi.btn, 0), "更换 WiFi");
        break;
    }

    case WIFI_MGR_FAILED: {
        char cur[33];
        const char *ssid = wifi_mgr_get_current_ssid(cur, sizeof(cur)) ? cur : "";
        lv_label_set_text(s_wifi.status, "连接失败");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(0xE8590C), 0);
        lv_obj_add_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "无法连接「%s」。\n\n请检查密码后重试。",
                 ssid);
        lv_label_set_text(s_wifi.info, buf);
        lv_obj_clear_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_wifi.btn, 0), "重新配网");
        break;
    }

    default:
        lv_label_set_text(s_wifi.status, "WiFi 配网");
        lv_obj_set_style_text_color(s_wifi.status, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_label_set_text(s_wifi.info, "配置你的 WiFi 网络。");
        lv_obj_add_flag(s_wifi.qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wifi.btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_wifi.btn, 0), "开始配网");
        break;
    }
}

static void poll_cb(lv_timer_t *t)
{
    bool vis = is_visible_chain(s_wifi.root);
    if (!vis) {
        s_was_visible = false;
        return;
    }
    bool first = !s_was_visible;
    s_was_visible = true;
    if (first || wifi_mgr_consume_changed()) {
        update_ui();
    }
}

static void root_delete_cb(lv_event_t *e)
{
    /* singleton timer: pause only when its owning page is destroyed (see the
     * same note in ui_gforce.c - deleting it here would kill the next page's
     * timer and leak a running one against freed widgets) */
    if (s_wifi.poll && lv_event_get_target(e) == s_wifi.poll_owner) {
        lv_timer_pause(s_wifi.poll);
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
bool ui_wifi_is_in_password(void)
{
    return false;   /* provisioning has no on-device password page */
}

void ui_wifi_back(void)
{
}

void ui_wifi_build(lv_obj_t *parent)
{
    /* NOTE: do NOT memset the whole struct here - s_wifi.poll is a singleton
     * that must survive across builds (it is paused when its page is deleted
     * and resumed on re-entry). Zeroing it would leak a running timer on every
     * settings build and let the stale timers write to freed widgets (UAF).
     * Every field below is assigned explicitly. */
    s_was_visible = false;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, 360, 290);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi.root = root;

    /* status text + on/off switch, centered as one row near the top. Keeping
     * the switch in the middle avoids the round screen clipping at the edge. */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 2);

    s_wifi.status = wl_label(row, "", UI_COLOR_TEXT, &misans_20);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 30);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_BG_ELEV), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    if (wifi_mgr_is_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi.sw = sw;

    /* QR code that encodes the open hotspot, so the phone joins by scanning
     * the watch screen */
    lv_obj_t *qr = lv_qrcode_create(root);
    lv_qrcode_set_size(qr, 96);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    char qrtext[48];
    snprintf(qrtext, sizeof(qrtext), "WIFI:T:nopass;S:%s;;", wifi_mgr_prov_ap_name());
    lv_qrcode_update(qr, qrtext, strlen(qrtext));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 44);
    s_wifi.qr = qr;

    s_wifi.info = wl_label(root, "", UI_COLOR_DIM, &misans_14);
    lv_label_set_long_mode(s_wifi.info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi.info, 280);
    lv_obj_align(s_wifi.info, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 200, 36);
    /* leave the bottom band (y~290..308) free for the "swipe to go back" hint */
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    wl_label(btn, "开始配网", 0x0A0E14, &misans_16);
    lv_obj_center(lv_obj_get_child(btn, 0));
    s_wifi.btn = btn;

    s_wifi.poll_owner = root;
    if (s_wifi.poll == NULL) {
        s_wifi.poll = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
    } else {
        lv_timer_reset(s_wifi.poll);
        lv_timer_resume(s_wifi.poll);
    }

    /* pause the poll timer only when this page's objects are destroyed */
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);

    update_ui();
}
