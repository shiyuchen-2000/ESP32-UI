/*
 * Settings app: a scrollable "waterfall" list of rounded entry cards. Tapping
 * a card opens that section's detail page: Brightness and Volume get working
 * sliders, the rest are placeholders. Uses a single screen with two views so
 * navigation stays inside the app.
 */
#include "ui_settings.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_wifi.h"
#include "backlight.h"
#include "audio.h"
#include "battery.h"
#include "auto_rotate.h"
#include "nav_ble.h"
#include "lvgl.h"
#include "nvs.h"
#include <stdio.h>

#define SEC_COUNT 7

enum {
    SEC_AUTOROTATE,   /* pinned first: the most-toggled setting, easiest to reach */
    SEC_WIFI,
    SEC_BLUETOOTH,
    SEC_BRIGHTNESS,
    SEC_VOLUME,
    SEC_BATTERY,
    SEC_ABOUT,
};

static const struct {
    const char *name;
    char  glyph;
    uint32_t color;
} s_sections[SEC_COUNT] = {
    { "自动旋转",     'R', 0x805AD5 },
    { "WiFi",         'W', 0x00D1B2 },
    { "蓝牙",         'B', 0x4C6EF5 },
    { "亮度",         'B', 0x2B6CB0 },
    { "音量",         'V', 0x2F9E44 },
    { "电池",         'P', 0xFF9F43 },
    { "关于",         'A', 0x7950F2 },
};

#define CARD_W   260   /* at x=50: spans x50..310, clear of the edge bands */
#define CARD_H   62
#define CARD_GAP 12
#define CARD_STRIDE (CARD_H + CARD_GAP)

/* Slider width keeps the knob (at either end) clear of the edge-swipe bands
 * (x<48 and x>312), so dragging it never triggers "back". */
#define SLIDER_W  220
#define SLIDER_H  26
#define KNOB_SZ   30

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_sec_view[SEC_COUNT]; /* per-section detail content */
static bool s_in_detail = false;
static int  s_volume = 0;               /* muted by default (matches audio.c) */

static void autorotate_cb(lv_event_t *e);   /* defined with the switch below */
static void build_about_view(lv_obj_t *view);

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    return l;
}

/* ------------------------------------------------------------------ */
/* slider behaviour                                                   */
/* ------------------------------------------------------------------ */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lab = (lv_obj_t *)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(lab, buf);
    esp_ui_set_backlight((uint8_t)v);
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lab = (lv_obj_t *)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(sl);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(lab, buf);
    s_volume = v;
    audio_set_volume(v);
}

/* Persist a brightness/volume value once the user releases the slider (NVS
 * lives on flash, so writing on every VALUE_CHANGED drag would wear it). */
static void save_ui_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* After the user lets go of the volume slider, sound a beep at the new
 * level so the change is audible, and remember it. */
static void volume_released_cb(lv_event_t *e)
{
    audio_play_beep();
    save_ui_u8("volume", (uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

/* Remember the brightness when the user lets go of its slider. */
static void brightness_released_cb(lv_event_t *e)
{
    save_ui_u8("bright", (uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

static lv_obj_t *make_slider(lv_obj_t *parent, int min, int max, int val,
                             uint32_t accent, lv_event_cb_t cb, lv_obj_t *label)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, SLIDER_W, SLIDER_H);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);

    /* track */
    lv_obj_set_style_bg_color(sl, lv_color_hex(UI_COLOR_BG_ELEV), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    /* filled part */
    lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    /* big thumb, easy to grab */
    lv_obj_set_style_bg_color(sl, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_width(sl, KNOB_SZ, LV_PART_KNOB);
    lv_obj_set_style_height(sl, KNOB_SZ, LV_PART_KNOB);

    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, label);
    return sl;
}

/* ------------------------------------------------------------------ */
/* navigation                                                         */
/* ------------------------------------------------------------------ */
static void section_cb(lv_event_t *e)
{
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *d_title = lv_obj_get_child(s_detail, 0);
    lv_label_set_text(d_title, s_sections[sec].name);
    lv_obj_set_style_text_color(d_title, lv_color_hex(s_sections[sec].color), 0);

    for (int i = 0; i < SEC_COUNT; i++) {
        if (i == sec) {
            lv_obj_clear_flag(s_sec_view[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sec_view[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_in_detail = true;

    /* sound the current volume level when the page opens */
    if (sec == SEC_VOLUME) {
        audio_play_beep();
    }
}

/* Settings owns its internal navigation: a WiFi password sub-page first, then
 * a detail page back to the list. Only when neither is up does the shell's
 * back fall through to going home. */
bool ui_settings_can_back(void)
{
    return ui_wifi_is_in_password() || s_in_detail;
}

void ui_settings_do_back(void)
{
    if (ui_wifi_is_in_password()) {
        ui_wifi_back();
    } else if (s_in_detail) {
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        s_in_detail = false;
    }
}

/* ------------------------------------------------------------------ */
/* views                                                              */
/* ------------------------------------------------------------------ */
static void build_list_view(lv_obj_t *scr)
{
    /* Cards sit at x=50..310, clear of the edge-swipe bands (x<48 / x>312) so
     * a drag on a card never misfires the back gesture. No icon chip: just a
     * name on the left and a ">" chevron on the right. */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, CARD_W, 300);
    lv_obj_set_pos(s_list, 50, 56);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *card = lv_button_create(s_list);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, 0, i * CARD_STRIDE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_BG_ELEV), 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        if (i != SEC_AUTOROTATE) {
            lv_obj_add_event_cb(card, section_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, s_sections[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &misans_16, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);

        if (i == SEC_AUTOROTATE) {
            /* Auto Rotate has no detail page: an inline on/off switch instead
             * of the chevron, so it toggles straight from the list. */
            lv_obj_t *sw = lv_switch_create(card);
            lv_obj_set_size(sw, 64, 36);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
            lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_BG_ELEV), LV_PART_MAIN);
            lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
            if (auto_rotate_is_enabled()) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw, autorotate_cb, LV_EVENT_VALUE_CHANGED, NULL);
        } else {
            lv_obj_t *chev = lv_label_create(card);
            lv_label_set_text(chev, ">");
            lv_obj_set_style_text_color(chev, lv_color_hex(UI_COLOR_DIM), 0);
            lv_obj_set_style_text_font(chev, &misans_20, 0);
            lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -16, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Battery page (read-only gauge info)                                */
/* ------------------------------------------------------------------ */
#define BATT_BAR_W 220
#define BATT_BAR_H 26

static lv_obj_t *s_batt_percent = NULL;
static lv_obj_t *s_batt_fill = NULL;
static lv_obj_t *s_batt_voltage = NULL;
static lv_obj_t *s_batt_status = NULL;
static lv_timer_t *s_batt_timer = NULL;
static lv_obj_t *s_batt_timer_owner = NULL;  /* view that owns the timer */

static bool ui_is_visible_chain(lv_obj_t *o)
{
    while (o) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        o = lv_obj_get_parent(o);
    }
    return true;
}

static void battery_timer_cb(lv_timer_t *t)
{
    if (!s_batt_percent || !ui_is_visible_chain(s_batt_percent)) {
        return;
    }
    int pct = battery_get_percent();
    int mv = battery_get_mv();
    char buf[32];

    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_batt_percent, buf);
        lv_obj_set_width(s_batt_fill, (lv_coord_t)(BATT_BAR_W * pct / 100));
    } else {
        lv_label_set_text(s_batt_percent, "--%");
        lv_obj_set_width(s_batt_fill, 0);
    }

    if (mv >= 0) {
        /* show the signed current too, so charging is easy to confirm by eye */
        snprintf(buf, sizeof(buf), "%d.%02d V  %+dmA", mv / 1000, (mv % 1000) / 10, battery_get_current_ma());
        lv_label_set_text(s_batt_voltage, buf);
    } else {
        lv_label_set_text(s_batt_voltage, "--");
    }

    /* BQ27220 has no CHG bit; battery_get_state() infers it from the current
     * sign, so a terminated charge reads "Full" instead of "Not charging". */
    const char *st;
    switch (battery_get_state()) {
    case BATT_STATE_CHARGING:     st = "充电中";     break;
    case BATT_STATE_FULL:         st = "已充满";         break;
    case BATT_STATE_UNKNOWN:      st = "未知";      break;
    default:                      st = "未充电"; break;
    }
    lv_label_set_text(s_batt_status, st);
}

static void battery_view_delete_cb(lv_event_t *e)
{
    /* singleton timer: pause only when its owning view is destroyed (see the
     * same note in ui_gforce.c - deleting it here would kill the next view's
     * timer and leak a running one against freed labels) */
    if (s_batt_timer && lv_event_get_target(e) == s_batt_timer_owner) {
        lv_timer_pause(s_batt_timer);
    }
}

static void build_battery_view(lv_obj_t *view)
{
    lv_obj_add_event_cb(view, battery_view_delete_cb, LV_EVENT_DELETE, NULL);

    s_batt_percent = make_label(view, "--%", UI_COLOR_TEXT, &misans_28);
    lv_obj_align(s_batt_percent, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *bar = lv_obj_create(view);
    lv_obj_set_size(bar, BATT_BAR_W, BATT_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BG_ELEV), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(bar);
    lv_obj_set_size(fill, 0, BATT_BAR_H);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    s_batt_fill = fill;

    s_batt_voltage = make_label(view, "--", UI_COLOR_DIM, &misans_16);
    lv_obj_align(s_batt_voltage, LV_ALIGN_TOP_MID, 0, 130);

    s_batt_status = make_label(view, "", UI_COLOR_FAINT, &misans_14);
    lv_obj_align(s_batt_status, LV_ALIGN_TOP_MID, 0, 158);

    s_batt_timer_owner = view;
    if (s_batt_timer == NULL) {
        s_batt_timer = lv_timer_create(battery_timer_cb, 1000, NULL);
    } else {
        lv_timer_reset(s_batt_timer);
        lv_timer_resume(s_batt_timer);
    }
    battery_timer_cb(s_batt_timer);
}

/* ------------------------------------------------------------------ */
/* Auto Rotate (inline switch on the settings list)                   */
/* ------------------------------------------------------------------ */
static void autorotate_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    auto_rotate_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* ------------------------------------------------------------------ */
/* Bluetooth page                                                     */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_bt_status = NULL;
static lv_obj_t *s_bt_switch = NULL;
static lv_obj_t *s_bt_name = NULL;
static lv_timer_t *s_bt_timer = NULL;
static lv_obj_t *s_bt_timer_owner = NULL;

static void bt_refresh(void)
{
    if (!s_bt_status || !s_bt_switch) {
        return;
    }
    bool en = nav_ble_is_enabled();
    if (en != lv_obj_has_state(s_bt_switch, LV_STATE_CHECKED)) {
        if (en) {
            lv_obj_add_state(s_bt_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_bt_switch, LV_STATE_CHECKED);
        }
    }

    const char *txt;
    uint32_t col;
    if (!en) {
        txt = "蓝牙已关闭";
        col = UI_COLOR_FAINT;
    } else if (nav_ble_is_connected()) {
        txt = "已连接手机";
        col = UI_COLOR_ACCENT;
    } else {
        txt = "可被发现";
        col = UI_COLOR_ACCENT;
    }
    lv_label_set_text(s_bt_status, txt);
    lv_obj_set_style_text_color(s_bt_status, lv_color_hex(col), 0);
    lv_label_set_text_fmt(s_bt_name, "设备名：%s", nav_ble_get_name());
}

static void bt_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    nav_ble_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    bt_refresh();
}

static void bt_timer_cb(lv_timer_t *t)
{
    (void)t;
    bt_refresh();
}

static void bt_view_delete_cb(lv_event_t *e)
{
    if (s_bt_timer && lv_event_get_target(e) == s_bt_timer_owner) {
        lv_timer_pause(s_bt_timer);
    }
}

static void build_bt_view(lv_obj_t *view)
{
    lv_obj_add_event_cb(view, bt_view_delete_cb, LV_EVENT_DELETE, NULL);

    /* status text + switch, centred as one row (same pattern as WiFi) */
    lv_obj_t *row = lv_obj_create(view);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 10);

    s_bt_status = make_label(row, "蓝牙", UI_COLOR_TEXT, &misans_20);

    s_bt_switch = lv_switch_create(row);
    lv_obj_set_size(s_bt_switch, 56, 30);
    lv_obj_set_style_bg_color(s_bt_switch, lv_color_hex(UI_COLOR_BG_ELEV), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bt_switch, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bt_switch, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_bt_switch, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bt_name = make_label(view, "", UI_COLOR_DIM, &misans_14);
    lv_obj_align(s_bt_name, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *hint = make_label(view, "打开后手机可在蓝牙列表中找到本设备", UI_COLOR_FAINT, &misans_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 300);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 96);

    s_bt_timer_owner = view;
    if (s_bt_timer == NULL) {
        s_bt_timer = lv_timer_create(bt_timer_cb, 1000, NULL);
    } else {
        lv_timer_reset(s_bt_timer);
        lv_timer_resume(s_bt_timer);
    }
    bt_refresh();
}

/* Content for one detail page. Brightness/Volume get a big slider with a
 * live value readout; WiFi gets the provisioning page; Battery gets the gauge. */
static void build_section_content(int sec, lv_obj_t *view)
{
    if (sec == SEC_BRIGHTNESS || sec == SEC_VOLUME) {
        int  lo  = (sec == SEC_BRIGHTNESS) ? 15 : 0;
        int  hi  = 100;
        int  cur = (sec == SEC_BRIGHTNESS) ? esp_ui_get_backlight() : s_volume;
        lv_event_cb_t cb = (sec == SEC_BRIGHTNESS) ? brightness_cb : volume_cb;

        /* live value readout */
        lv_obj_t *val = make_label(view, "", UI_COLOR_TEXT, &misans_28);
        lv_obj_align(val, LV_ALIGN_TOP_MID, 0, 16);
        char buf[8];
        snprintf(buf, sizeof(buf), (sec == SEC_BRIGHTNESS) ? "%d%%" : "%d", cur);
        lv_label_set_text(val, buf);

        lv_obj_t *sl = make_slider(view, lo, hi, cur, s_sections[sec].color, cb, val);
        lv_obj_align(sl, LV_ALIGN_CENTER, 0, 0);

        lv_event_cb_t rel = (sec == SEC_BRIGHTNESS) ? brightness_released_cb
                           : (sec == SEC_VOLUME)     ? volume_released_cb
                           : NULL;
        if (rel) {
            lv_obj_add_event_cb(sl, rel, LV_EVENT_RELEASED, NULL);
        }

        char range[16];
        snprintf(range, sizeof(range), "%d - %d", lo, hi);
        lv_obj_t *hint = make_label(view, range, UI_COLOR_FAINT, &misans_14);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
    } else if (sec == SEC_BLUETOOTH) {
        build_bt_view(view);
    } else if (sec == SEC_WIFI) {
        ui_wifi_build(view);
    } else if (sec == SEC_BATTERY) {
        build_battery_view(view);
    } else if (sec == SEC_ABOUT) {
        build_about_view(view);
    } else {
        lv_obj_t *ph = make_label(view, "内容开发中", UI_COLOR_DIM, &misans_16);
        lv_obj_center(ph);
    }
}

/* About: three credit lines, packed toward the top of the content view (the
 * view starts at screen y=56, below the section title; the bottom "swipe back"
 * hint sits at ~294, so nothing below ~240 screen px). Brand names keep their
 * own colours (Claude orange / DeepSeek blue), so the second line is a flex
 * row of labels (a single label can't mix colours). */
static void build_about_view(lv_obj_t *view)
{
    /* Powered By */
    lv_obj_t *kicker = make_label(view, "Powered By", UI_COLOR_DIM, &misans_16);
    lv_obj_align(kicker, LV_ALIGN_TOP_MID, 0, 24);

    /* ClaudeCode & DeepSeek V4.1 */
    lv_obj_t *row = lv_obj_create(view);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *cc = lv_label_create(row);
    lv_label_set_text(cc, "ClaudeCode");
    lv_obj_set_style_text_font(cc, &misans_20, 0);
    lv_obj_set_style_text_color(cc, lv_color_hex(0xD97757), 0);   /* Claude orange */

    lv_obj_t *amp = lv_label_create(row);
    lv_label_set_text(amp, " & ");
    lv_obj_set_style_text_font(amp, &misans_16, 0);
    lv_obj_set_style_text_color(amp, lv_color_hex(UI_COLOR_DIM), 0);

    lv_obj_t *ds = lv_label_create(row);
    lv_label_set_text(ds, "DeepSeek V4.1");
    lv_obj_set_style_text_font(ds, &misans_20, 0);
    lv_obj_set_style_text_color(ds, lv_color_hex(0x4D6BFE), 0);   /* DeepSeek blue */

    /* Designed by SYC */
    lv_obj_t *by = make_label(view, "Designed by SYC", UI_COLOR_TEXT, &misans_20);
    lv_obj_align(by, LV_ALIGN_TOP_MID, 0, 150);
}

static void build_detail_view(lv_obj_t *scr)
{
    s_detail = lv_obj_create(scr);
    lv_obj_set_size(s_detail, 360, 360);
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_radius(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);
    lv_obj_set_scrollbar_mode(s_detail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);

    /* child 0 = detail title (updated per section) */
    lv_obj_t *title = make_label(s_detail, "WiFi", 0x00D1B2, &misans_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *line = lv_obj_create(s_detail);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 180, 1);
    lv_obj_set_pos(line, 90, 46);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);

    /* one content view per section, shown when that card is tapped */
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *view = lv_obj_create(s_detail);
        lv_obj_set_size(view, 360, 290);
        lv_obj_set_pos(view, 0, 56);
        lv_obj_set_style_bg_opa(view, 0, 0);
        lv_obj_set_style_border_width(view, 0, 0);
        lv_obj_set_style_pad_all(view, 0, 0);
        lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(view, LV_OBJ_FLAG_SCROLLABLE);
        s_sec_view[i] = view;
        build_section_content(i, view);
        lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    }

    /* Bottom hint: keep it high enough that the round screen (center 180,180,
     * radius 168) doesn't clip it - at y~308 the visible width is still wide. */
    lv_obj_t *hint = make_label(s_detail, "从屏幕边缘滑动返回", UI_COLOR_FAINT, &misans_14);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -52);

    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_settings_create(int id)
{
    s_in_detail = false;
    (void)id;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    lv_obj_t *title = make_label(scr, "设置", UI_COLOR_TEXT, &misans_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 180, 1);
    lv_obj_set_pos(line, 90, 46);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);

    build_list_view(scr);
    build_detail_view(scr);

    return scr;
}
