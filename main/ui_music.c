/*
 * Music remote (occupies the old "信息" slot).
 *
 * The phone already speaks a small music protocol over the same BLE text
 * channel (MusicBridge.kt):
 *   watch -> phone   "T,M,P" previous, "T,M,T" play/pause, "T,M,N" next,
 *                    "T,M,Q" request the current state
 *   phone -> watch   "#Z,have,playing,actions"  (#ZACK,2 ok / #ZACK,5 no player)
 *
 * Layout: one large play/pause button in the middle, previous and next at the
 * sides. No track name. Every action shows a short hint and the play/pause
 * icon follows the phone's real state (with an optimistic flip on press).
 */
#include "ui_music.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_music_icons.h"
#include "nav_proto.h"
#include "nav_ble.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define MUSIC_COLOR 0x9C36B5
#define SIDE_BG     0x2A1F33
#define HINT_MS     1200

static lv_obj_t *s_owner;
static lv_timer_t *s_timer;
static lv_obj_t *s_play_img;
static lv_obj_t *s_hint;
static lv_obj_t *s_btns[3];
static bool     s_playing;       /* icon currently shown */
static bool     s_hint_live;
static uint32_t s_hint_at;
static int      s_last_ack_seq;
static lv_opa_t s_last_opa;
static uint32_t s_icon_hold;     /* optimistic play/pause icon until this time */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_hint(const char *text)
{
    lv_label_set_text(s_hint, text);
    s_hint_live = text[0] != '\0';
    s_hint_at = now_ms();
}

static void show_play_icon(bool playing)
{
    if (playing == s_playing) {
        return;
    }
    s_playing = playing;
    lv_image_set_src(s_play_img, playing ? &s_icon_pause : &s_icon_play);
}

/* "T,M,<cmd>" to the phone. False when the link is down. */
static bool send_cmd(char cmd, const char *hint)
{
    nav_state_t st;
    nav_proto_get(&st);
    if (!st.connected) {
        set_hint("未连接手机");
        return false;
    }
    char b[8];
    snprintf(b, sizeof(b), "T,M,%c", cmd);
    nav_ble_notify(b);
    set_hint(hint);
    return true;
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    send_cmd('P', "上一首");
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    send_cmd('N', "下一首");
}

static void toggle_cb(lv_event_t *e)
{
    (void)e;
    bool was = s_playing;
    if (send_cmd('T', was ? "暂停" : "播放")) {
        /* optimistic flip, held for a moment so the 200ms state poll does not
         * snap it back before the phone reports the new state */
        show_play_icon(!was);
        s_icon_hold = now_ms() + 800;
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    nav_state_t st;
    nav_proto_get(&st);

    if (now_ms() >= s_icon_hold) {
        show_play_icon(st.music_playing);
    }

    /* the phone answers every command; 5 means "no controllable player" */
    if (st.music_ack_seq != s_last_ack_seq) {
        s_last_ack_seq = st.music_ack_seq;
        if (st.music_ack == 5) {
            set_hint("无音乐播放");
            s_icon_hold = 0;
            show_play_icon(false);
        }
    }

    /* dim the buttons when there is nothing to control */
    lv_opa_t opa = (st.connected && st.music_have) ? LV_OPA_COVER : LV_OPA_40;
    if (opa != s_last_opa) {
        s_last_opa = opa;
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_opa(s_btns[i], opa, 0);
        }
    }

    if (s_hint_live && now_ms() - s_hint_at > HINT_MS) {
        s_hint_live = false;
        lv_label_set_text(s_hint, "");
    }
}

static void screen_gone_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_owner) {
        return;
    }
    s_owner = NULL;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_play_img = NULL;
    s_hint = NULL;
    for (int i = 0; i < 3; i++) {
        s_btns[i] = NULL;
    }
}

static lv_obj_t *make_button(lv_obj_t *scr, int size, int x, int y,
                             uint32_t bg, uint32_t bg_pressed, bool ring)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg_pressed), LV_STATE_PRESSED);
    if (ring) {
        lv_obj_set_style_border_color(b, lv_color_hex(MUSIC_COLOR), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_60, 0);
    }
    return b;
}

lv_obj_t *ui_music_create(int id)
{
    (void)id;

    if (s_timer) {   /* a stale instance that was never unloaded */
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    nav_state_t st;
    nav_proto_get(&st);
    s_playing = st.music_playing;
    s_hint_live = false;
    s_last_opa = LV_OPA_COVER;
    s_last_ack_seq = st.music_ack_seq;   /* do not react to an old #ZACK */
    s_icon_hold = 0;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* side buttons */
    s_btns[0] = make_button(scr, 92, 22, 134, SIDE_BG, 0x3D2C4A, true);
    s_btns[2] = make_button(scr, 92, 246, 134, SIDE_BG, 0x3D2C4A, true);
    /* centre play/pause, bigger and filled with the accent colour */
    s_btns[1] = make_button(scr, 112, 124, 124, MUSIC_COLOR, 0xB44FD0, false);

    lv_obj_t *ip = lv_image_create(s_btns[0]);
    lv_image_set_src(ip, &s_icon_prev);
    lv_obj_center(ip);

    lv_obj_t *in = lv_image_create(s_btns[2]);
    lv_image_set_src(in, &s_icon_next);
    lv_obj_center(in);

    s_play_img = lv_image_create(s_btns[1]);
    lv_image_set_src(s_play_img, st.music_playing ? &s_icon_pause : &s_icon_play);
    lv_obj_center(s_play_img);

    lv_obj_add_event_cb(s_btns[0], prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btns[1], toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btns[2], next_cb, LV_EVENT_CLICKED, NULL);

    /* transient action feedback ("上一首" / "播放" / "无音乐播放" ...) */
    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_font(s_hint, &misans_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 34);

    s_owner = scr;
    s_timer = lv_timer_create(tick_cb, 200, NULL);
    lv_obj_add_event_cb(scr, screen_gone_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(scr, screen_gone_cb, LV_EVENT_DELETE, NULL);

    /* ask the phone for the current state so the icon is right immediately */
    if (st.connected) {
        nav_ble_notify("T,M,Q");
    }
    return scr;
}
