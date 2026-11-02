/*
 * Navigation app: MotoNav protocol display.
 *
 * Two layouts share the same widgets:
 *   - no map yet: a big turn arrow in the centre (the simple stage-1 screen);
 *   - map active: a 360×360 RGB565 canvas in PSRAM rendered by nav_map.c and
 *     the turn/distance shown as a compact overlay on top.
 */
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "clock_faces.h"   /* hooge36 */
#include "nav_proto.h"
#include "nav_map.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/* Black-outlined text for bitmap fonts. LVGL's text_outline_stroke only
 * works with vector fonts, so each glyph is drawn 8 times at ±2 px in black
 * under the coloured copy (the classic outlined-text trick). Each outlined
 * text is a tiny auto-sized container holding the 9 labels. */
typedef struct {
    lv_obj_t *box;
    lv_obj_t *main;
    lv_obj_t *ol[8];
} outlined_label_t;

static const int OL_DX[8] = { -2, 0, 2, -2, 2, -2, 0, 2 };
static const int OL_DY[8] = { -2, -2, -2, 0, 0, 2, 2, 2 };

static void outlined_create(outlined_label_t *t, lv_obj_t *parent,
                            const lv_font_t *font, uint32_t color)
{
    t->box = lv_obj_create(parent);
    lv_obj_remove_style_all(t->box);
    lv_obj_clear_flag(t->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(t->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    for (int i = 0; i < 8; i++) {
        t->ol[i] = lv_label_create(t->box);
        lv_obj_set_pos(t->ol[i], 2 + OL_DX[i], 2 + OL_DY[i]);
        lv_label_set_text(t->ol[i], "");
        lv_obj_set_style_text_font(t->ol[i], font, 0);
        lv_obj_set_style_text_color(t->ol[i], lv_color_hex(0x000000), 0);
    }
    t->main = lv_label_create(t->box);
    lv_obj_set_pos(t->main, 2, 2);
    lv_label_set_text(t->main, "");
    lv_obj_set_style_text_font(t->main, font, 0);
    lv_obj_set_style_text_color(t->main, lv_color_hex(color), 0);
}

/* lv_label_set_text() frees and reallocates even when the string is unchanged,
 * so a 10 Hz refresh of 11 labels churned the heap for nothing. */
static void label_set(lv_obj_t *l, const char *text)
{
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(l, text);
}

static void outlined_set(outlined_label_t *t, const char *text)
{
    const char *cur = lv_label_get_text(t->main);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(t->main, text);
    for (int i = 0; i < 8; i++) {
        lv_label_set_text(t->ol[i], text);
    }
}

static void outlined_hide(outlined_label_t *t, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(t->box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(t->box, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *s_status;   /* big centred waiting/connected text */
static lv_obj_t *s_hint;     /* subtitle under it */
static lv_obj_t *s_arrow_box;
static outlined_label_t s_dist_t;   /* top distance: white + black outline */
static lv_obj_t *s_speed;           /* green #00CD00 on a small chip */
static lv_obj_t *s_trip;
static lv_obj_t *s_speed_box;
static lv_obj_t *s_trip_box;
static lv_obj_t *s_shaft;    /* white inner arrow lines */
static lv_obj_t *s_head1;
static lv_obj_t *s_head2;
static lv_obj_t *s_shaft_o;  /* black outline pass */
static lv_obj_t *s_head1_o;
static lv_obj_t *s_head2_o;
static lv_obj_t *s_arrive;       /* "已到达" full-screen prompt */
static lv_obj_t *s_arrive_hint;
static lv_obj_t *s_map;
static uint16_t *s_map_buf;
static bool      s_map_loaded;    /* at least one map frame has been rendered */
static uint32_t  s_map_wait_at;   /* ms when we started waiting for the first map */
static uint32_t  s_render_sig;    /* state the canvas currently shows */
static bool      s_render_sig_valid;
static uint32_t  s_status_color;
static bool      s_status_color_set;

#define NAV_STALE_MS 6000   /* same timeout as the reference firmware */

static nav_turn_t s_turn = NAV_TURN_NONE;
static bool s_map_layout = false;

/* Arrow geometry lives in a 160x160 box. lv_line keeps the pointer, so the
 * arrays must have static storage duration. The V is kept wide and the
 * shaft long so the white arrow stays open and readable at the small
 * map-layout scale instead of collapsing into a blob. */
static const lv_point_precise_t P_STRAIGHT_S[] = { {80,152}, {80,48} };
static const lv_point_precise_t P_STRAIGHT_H1[] = { {54,74}, {80,48} };
static const lv_point_precise_t P_STRAIGHT_H2[] = { {106,74}, {80,48} };

static const lv_point_precise_t P_RIGHT_S[] = { {62,152}, {62,80}, {116,80} };
static const lv_point_precise_t P_RIGHT_H1[] = { {92,56}, {116,80} };
static const lv_point_precise_t P_RIGHT_H2[] = { {92,104}, {116,80} };

static const lv_point_precise_t P_LEFT_S[] = { {98,152}, {98,80}, {44,80} };
static const lv_point_precise_t P_LEFT_H1[] = { {68,56}, {44,80} };
static const lv_point_precise_t P_LEFT_H2[] = { {68,104}, {44,80} };

static const lv_point_precise_t P_SRIGHT_S[] = { {68,152}, {104,66} };
static const lv_point_precise_t P_SRIGHT_H1[] = { {84,52}, {104,66} };
static const lv_point_precise_t P_SRIGHT_H2[] = { {116,50}, {104,66} };

static const lv_point_precise_t P_SLEFT_S[] = { {92,152}, {56,66} };
static const lv_point_precise_t P_SLEFT_H1[] = { {76,52}, {56,66} };
static const lv_point_precise_t P_SLEFT_H2[] = { {44,50}, {56,66} };

static const lv_point_precise_t P_UTURN_S[] = { {58,152}, {58,70}, {102,70}, {102,108} };
static const lv_point_precise_t P_UTURN_H1[] = { {82,88}, {102,108} };
static const lv_point_precise_t P_UTURN_H2[] = { {122,88}, {102,108} };

/* scaled copies (filled at runtime) */
static lv_point_precise_t s_shaft_pts[8];
static lv_point_precise_t s_h1_pts[2];
static lv_point_precise_t s_h2_pts[2];

static lv_timer_t *s_timer = NULL;
static lv_obj_t *s_timer_owner = NULL;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Green text on a very small, subtle dark chip (tight padding, low opacity,
 * auto-sized to hug the text). Returns the label; the chip is its parent. */
static lv_obj_t *make_small_chip(lv_obj_t *parent, const lv_font_t *font,
                                 uint32_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, 90, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_hor(box, 8, 0);
    lv_obj_set_style_pad_ver(box, 3, 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void set_scaled(lv_point_precise_t *dst, const lv_point_precise_t *src,
                       int n, float k)
{
    for (int i = 0; i < n; i++) {
        dst[i].x = 80.0f + (src[i].x - 80.0f) * k;
        dst[i].y = 80.0f + (src[i].y - 80.0f) * k;
    }
}

static void apply_turn(nav_turn_t turn)
{
    s_turn = turn;
    const lv_point_precise_t *s = NULL, *h1 = NULL, *h2 = NULL;
    int sn = 0;
    switch (turn) {
    case NAV_TURN_STRAIGHT:    s = P_STRAIGHT_S; sn = 2; h1 = P_STRAIGHT_H1; h2 = P_STRAIGHT_H2; break;
    case NAV_TURN_RIGHT:       s = P_RIGHT_S;    sn = 3; h1 = P_RIGHT_H1;    h2 = P_RIGHT_H2;    break;
    case NAV_TURN_LEFT:        s = P_LEFT_S;     sn = 3; h1 = P_LEFT_H1;     h2 = P_LEFT_H2;     break;
    case NAV_TURN_SLIGHT_RIGHT:s = P_SRIGHT_S;   sn = 2; h1 = P_SRIGHT_H1;   h2 = P_SRIGHT_H2;   break;
    case NAV_TURN_SLIGHT_LEFT: s = P_SLEFT_S;    sn = 2; h1 = P_SLEFT_H1;    h2 = P_SLEFT_H2;    break;
    case NAV_TURN_UTURN:       s = P_UTURN_S;    sn = 4; h1 = P_UTURN_H1;    h2 = P_UTURN_H2;    break;
    default:
        lv_obj_add_flag(s_arrow_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* line width is independent of the point scale: thinner in the map
     * layout so the small arrow stays open instead of blobbed */
    lv_coord_t w = s_map_layout ? 9 : 15;
    float k = s_map_layout ? 0.66f : 1.0f;
    set_scaled(s_shaft_pts, s, sn, k);
    set_scaled(s_h1_pts, h1, 2, k);
    set_scaled(s_h2_pts, h2, 2, k);
    lv_obj_t *inners[] = { s_shaft, s_head1, s_head2 };
    lv_obj_t *outers[] = { s_shaft_o, s_head1_o, s_head2_o };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_line_width(inners[i], w, 0);
        lv_obj_set_style_line_width(outers[i], w + 6, 0);
    }
    lv_line_set_points(s_shaft, s_shaft_pts, sn);
    lv_line_set_points(s_head1, s_h1_pts, 2);
    lv_line_set_points(s_head2, s_h2_pts, 2);
    lv_line_set_points(s_shaft_o, s_shaft_pts, sn);
    lv_line_set_points(s_head1_o, s_h1_pts, 2);
    lv_line_set_points(s_head2_o, s_h2_pts, 2);
    lv_obj_clear_flag(s_arrow_box, LV_OBJ_FLAG_HIDDEN);
}

/* Move the HUD between the simple (no map) and map layouts. */
static void set_layout(bool map_layout)
{
    if (map_layout == s_map_layout) {
        return;
    }
    s_map_layout = map_layout;
    if (map_layout) {
        /* turn arrow on the left, vertically centred (the player marker is
         * the centred one, drawn by nav_map.c at the screen centre) */
        lv_obj_align(s_arrow_box, LV_ALIGN_CENTER, -90, 0);
        lv_obj_align(s_dist_t.box, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_align(s_speed_box, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_align(s_trip_box, LV_ALIGN_BOTTOM_MID, 0, -50);
    } else {
        lv_obj_align(s_arrow_box, LV_ALIGN_CENTER, 0, -30);
        lv_obj_align(s_dist_t.box, LV_ALIGN_CENTER, 0, 60);
        lv_obj_align(s_speed_box, LV_ALIGN_CENTER, 0, 95);
        lv_obj_align(s_trip_box, LV_ALIGN_CENTER, 0, 128);
    }
    apply_turn(s_turn);
}

/* Big centred waiting/connected screen: nothing else is shown. */
static void show_status(const char *title, const char *hint, uint32_t color)
{
    label_set(s_status, title);
    if (!s_status_color_set || s_status_color != color) {
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
        s_status_color = color;
        s_status_color_set = true;
    }
    label_set(s_hint, hint);
    lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrow_box, LV_OBJ_FLAG_HIDDEN);
    outlined_hide(&s_dist_t, true);
    lv_obj_add_flag(s_speed_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_trip_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrive, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrive_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_map) {
        lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Everything the rendered canvas depends on. Position/heading arrive at ~1 Hz,
 * so re-rendering the full 360x360 frame every 100 ms tick was 10x wasted work
 * (and, with auto-rotate on, a full-screen pixel rotation each time). */
static uint32_t map_sig(const nav_state_t *st)
{
    uint32_t v = nav_map_version();
    v = v * 31u + (uint32_t)st->pos_frame;
    v = v * 31u + (uint32_t)st->pos_x_dm;
    v = v * 31u + (uint32_t)st->pos_y_dm;
    v = v * 31u + (uint32_t)st->heading;
    v = v * 31u + (uint32_t)st->speed_kmh;
    v = v * 31u + (uint32_t)st->theme;
    v = v * 31u + (st->night ? 1u : 0u);
    v = v * 31u + (uint32_t)st->trail;
    return v;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    nav_state_t st;
    nav_proto_get(&st);

    if (!st.connected) {
        s_map_wait_at = 0;   /* do not carry a half-expired wait across a reconnect */
        show_status("等待连接", "手机打开 MotoNav 并连接手表", UI_COLOR_DIM);
        return;
    }
    if (!st.active) {
        s_map_wait_at = 0;
        show_status("等待手机开始导航", "已连接", UI_COLOR_ACCENT);
        return;
    }

    /* The link can stay up while the app stops sending (killed in the
     * background, GPS lost). Without this check the last frame stayed on
     * screen forever, looking like a live navigation. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_rx = nav_proto_last_rx_ms();
    if (last_rx != 0 && now_ms - last_rx > NAV_STALE_MS) {
        s_map_wait_at = 0;
        s_render_sig_valid = false;   /* redraw as soon as data returns */
        show_status("数据中断", "手机导航已停止更新", 0xFF3B30);
        return;
    }

    /* Navigation is running: the status screen is no longer needed. */
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    /* Arrived: full-screen prompt until the phone ends the session. */
    if (st.turn == NAV_TURN_ARRIVE) {
        lv_obj_clear_flag(s_arrive, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_arrive_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_arrow_box, LV_OBJ_FLAG_HIDDEN);
        outlined_hide(&s_dist_t, true);
        lv_obj_add_flag(s_speed_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_trip_box, LV_OBJ_FLAG_HIDDEN);
        if (s_map) {
            lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_add_flag(s_arrive, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_arrive_hint, LV_OBJ_FLAG_HIDDEN);

    bool map_expected = st.map_on;
    bool map_ok = false;
    if (map_expected && st.pos_frame != 0 && nav_map_has_map() && s_map_buf) {
        uint32_t sig = map_sig(&st);
        bool stale_frame = s_render_sig_valid && sig == s_render_sig &&
                           !lv_obj_has_flag(s_map, LV_OBJ_FLAG_HIDDEN);
        if (stale_frame) {
            map_ok = true;   /* the canvas already shows exactly this state */
        } else if (nav_map_render(s_map_buf, UI_SCREEN_SIZE, UI_SCREEN_SIZE, &st)) {
            s_render_sig = sig;
            s_render_sig_valid = true;
            map_ok = true;
            lv_obj_invalidate(s_map);
        }
        if (map_ok) {
            s_map_loaded = true;
            s_map_wait_at = 0;
            lv_obj_clear_flag(s_map, LV_OBJ_FLAG_HIDDEN);
            set_layout(true);
        }
    }
    if (!map_ok) {
        if (map_expected && !s_map_loaded) {
            /* The phone is streaming a map image but it has not arrived yet.
             * Show a loading hint instead of the misleading turn-arrow screen,
             * but give up after 6 s and fall back to the arrow. */
            if (s_map_wait_at == 0) {
                s_map_wait_at = now_ms;
            }
            if (now_ms - s_map_wait_at < 6000) {
                show_status("地图加载中", "已连接", UI_COLOR_ACCENT);
                return;
            }
            s_map_wait_at = 0;
        }
        if (map_expected && s_map_loaded) {
            /* a map was shown before: keep the last frame on screen while the
             * next one loads instead of flashing back to the arrow */
            lv_obj_clear_flag(s_map, LV_OBJ_FLAG_HIDDEN);
            set_layout(true);
        } else {
            if (s_map) {
                lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
            }
            set_layout(false);
        }
    }

    nav_turn_t want = st.turn;
    if (want != s_turn) {
        apply_turn(want);
    }
    if (st.turn != NAV_TURN_NONE) {
        lv_obj_clear_flag(s_arrow_box, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[40];
    if (st.turn == NAV_TURN_ARRIVE) {
        snprintf(buf, sizeof(buf), "到达");
    } else if (st.distance_m >= 1000) {
        snprintf(buf, sizeof(buf), "%.1fkm", st.distance_m / 1000.0f);
    } else {
        snprintf(buf, sizeof(buf), "%dm", st.distance_m);
    }
    outlined_set(&s_dist_t, buf);
    outlined_hide(&s_dist_t, false);

    /* Keep the chip visible at a standstill ("0 km/h"): hiding it made the
     * bottom HUD jump every time the rider stopped at a light. */
    snprintf(buf, sizeof(buf), "%d km/h", st.speed_kmh);
    label_set(s_speed, buf);
    lv_obj_clear_flag(s_speed_box, LV_OBJ_FLAG_HIDDEN);

    if (st.has_trip) {
        snprintf(buf, sizeof(buf), "剩余 %.1fkm · %02d:%02d",
                 st.remain_m / 1000.0f, st.eta_hhmm / 100, st.eta_hhmm % 100);
        label_set(s_trip, buf);
        lv_obj_clear_flag(s_trip_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_trip_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_delete_cb(lv_event_t *e)
{
    if (s_timer && lv_event_get_target(e) == s_timer_owner) {
        lv_timer_pause(s_timer);
    }
}

lv_obj_t *ui_nav_create(int id)
{
    (void)id;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    /* map canvas first so every HUD widget stays above it */
    if (!s_map_buf) {
        s_map_buf = heap_caps_aligned_alloc(16, UI_SCREEN_SIZE * UI_SCREEN_SIZE * 2,
                                            MALLOC_CAP_SPIRAM);
    }
    if (s_map_buf) {
        s_map = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_map, s_map_buf, UI_SCREEN_SIZE, UI_SCREEN_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_map, 0, 0);
        lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
    }

    s_status = make_label(scr, &misans_28, UI_COLOR_TEXT);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -18);

    s_hint = make_label(scr, &misans_14, UI_COLOR_FAINT);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 30);

    s_arrow_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_arrow_box);
    lv_obj_set_size(s_arrow_box, 160, 160);
    lv_obj_align(s_arrow_box, LV_ALIGN_CENTER, 0, -40);
    lv_obj_clear_flag(s_arrow_box, LV_OBJ_FLAG_SCROLLABLE);

    /* arrow: black outline lines under the white inner lines */
    s_shaft_o = lv_line_create(s_arrow_box);
    s_head1_o = lv_line_create(s_arrow_box);
    s_head2_o = lv_line_create(s_arrow_box);
    s_shaft = lv_line_create(s_arrow_box);
    s_head1 = lv_line_create(s_arrow_box);
    s_head2 = lv_line_create(s_arrow_box);
    lv_obj_t *outers[] = { s_shaft_o, s_head1_o, s_head2_o };
    lv_obj_t *inners[] = { s_shaft, s_head1, s_head2 };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_pos(outers[i], 0, 0);
        lv_obj_set_style_line_width(outers[i], 21, 0);
        lv_obj_set_style_line_rounded(outers[i], true, 0);
        lv_obj_set_style_line_color(outers[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_line_opa(outers[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(inners[i], 0, 0);
        lv_obj_set_style_line_width(inners[i], 15, 0);
        lv_obj_set_style_line_rounded(inners[i], true, 0);
        lv_obj_set_style_line_color(inners[i], lv_color_white(), 0);
        lv_obj_set_style_line_opa(inners[i], LV_OPA_COVER, 0);
    }

    outlined_create(&s_dist_t, scr, &hooge36, UI_COLOR_TEXT);
    lv_obj_align(s_dist_t.box, LV_ALIGN_CENTER, 0, 60);

    s_speed = make_small_chip(scr, &misans_20, UI_COLOR_TEXT);
    s_speed_box = lv_obj_get_parent(s_speed);
    lv_obj_align(s_speed_box, LV_ALIGN_CENTER, 0, 95);

    s_trip = make_small_chip(scr, &misans_16, UI_COLOR_TEXT);
    s_trip_box = lv_obj_get_parent(s_trip);
    lv_obj_align(s_trip_box, LV_ALIGN_CENTER, 0, 128);

    s_arrive = make_label(scr, &misans_28, 0x2F9E44);
    lv_obj_set_style_text_align(s_arrive, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_arrive, LV_ALIGN_CENTER, 0, -26);
    lv_label_set_text(s_arrive, "已到达");
    lv_obj_add_flag(s_arrive, LV_OBJ_FLAG_HIDDEN);

    s_arrive_hint = make_label(scr, &misans_16, UI_COLOR_DIM);
    lv_obj_set_style_text_align(s_arrive_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_arrive_hint, LV_ALIGN_CENTER, 0, 22);
    lv_label_set_text(s_arrive_hint, "导航结束");
    lv_obj_add_flag(s_arrive_hint, LV_OBJ_FLAG_HIDDEN);

    outlined_hide(&s_dist_t, true);
    lv_obj_add_flag(s_speed_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_trip_box, LV_OBJ_FLAG_HIDDEN);

    s_map_layout = false;
    s_turn = NAV_TURN_NONE;
    s_render_sig_valid = false;   /* the shared canvas may hold an old frame */
    s_status_color_set = false;
    apply_turn(NAV_TURN_NONE);

    s_timer_owner = scr;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(tick_cb, 100, NULL);
    } else {
        lv_timer_reset(s_timer);
        lv_timer_resume(s_timer);
    }
    tick_cb(NULL);

    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}
