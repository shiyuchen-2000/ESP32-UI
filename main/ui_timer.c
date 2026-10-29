/*
 * Racing stopwatch (replaces the old "计时" placeholder app).
 *
 * Layout comes from DEV/Timer demo/赛车秒表.html:
 *   - navy round background, orange hooge digits, red separator line;
 *   - MM:SS:CC main time, three lap rows (newest on top, 35/25/15 px);
 *   - round P (pause/resume) and R (reset / clear history) buttons.
 *
 * Interaction:
 *   - tap the screen: start from 00:00:00; tap again to pause; tap again to
 *     resume;
 *   - P: record a lap (only while the stopwatch is running);
 *   - R: first press stops and zeroes the time (laps stay), a second press
 *     clears the lap history.
 *
 * Leaving the app tears the timer down and resets everything, so it never
 * keeps counting in the background.
 */
#include "ui_timer.h"
#include "clock_faces.h"   /* hooge55 / 36 / 25 / 17 */
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define TIMER_BG     0x0C3566
#define TIMER_ACCENT 0xF4A32A
#define TIMER_RED    0xEA1616

#define TICK_MS      20      /* 50 Hz: enough to show hundredths smoothly */
#define LAP_MAX      3
#define TIME_MAX_MS  5999990u /* clamp at 99:59:99 (the display is 8 chars) */

static lv_obj_t *s_owner;       /* the screen that owns the running timer */
static lv_timer_t *s_timer;
static lv_obj_t *s_time_lab;
static lv_obj_t *s_lap_lab[LAP_MAX];

static bool     s_running;
static bool     s_reset_armed;  /* one R press done: the next one clears laps */
static uint64_t s_seg_start_us; /* esp_timer when the current segment started */
static uint32_t s_acc_ms;       /* time accumulated before the current segment */
static uint32_t s_laps[LAP_MAX];
static int      s_lap_count;

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */
static uint32_t elapsed_ms(void)
{
    uint32_t ms = s_acc_ms;
    if (s_running) {
        ms += (uint32_t)((esp_timer_get_time() - s_seg_start_us) / 1000);
    }
    return ms > TIME_MAX_MS ? TIME_MAX_MS : ms;
}

static void fmt_time(char *b, size_t n, uint32_t ms)
{
    if (ms > TIME_MAX_MS) {
        ms = TIME_MAX_MS;
    }
    snprintf(b, n, "%02u:%02u:%02u",
             (unsigned)(ms / 60000u),
             (unsigned)((ms / 1000u) % 60u),
             (unsigned)((ms / 10u) % 100u));
}

static void set_text_if_changed(lv_obj_t *l, const char *text)
{
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(l, text);
}

static void show_time(void)
{
    if (!s_time_lab) {
        return;
    }
    char b[16];
    fmt_time(b, sizeof(b), elapsed_ms());
    set_text_if_changed(s_time_lab, b);
}

static void show_laps(void)
{
    char b[16];
    for (int i = 0; i < LAP_MAX; i++) {
        if (!s_lap_lab[i]) {
            continue;
        }
        if (i < s_lap_count) {
            fmt_time(b, sizeof(b), s_laps[i]);
            set_text_if_changed(s_lap_lab[i], b);
        } else {
            set_text_if_changed(s_lap_lab[i], "");
        }
    }
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */
/* Screen tap: start / pause / resume. */
static void do_tap(void)
{
    if (s_running) {
        s_acc_ms = elapsed_ms();
        s_running = false;
    } else {
        if (elapsed_ms() == 0) {
            s_acc_ms = 0;
        }
        s_seg_start_us = esp_timer_get_time();
        s_running = true;
        s_reset_armed = false;
    }
    show_time();
}

/* P: record the current time as a lap. Nothing to record while paused. */
static void do_lap(void)
{
    if (!s_running) {
        return;
    }
    /* newest first, the oldest one drops off */
    for (int i = LAP_MAX - 1; i > 0; i--) {
        s_laps[i] = s_laps[i - 1];
    }
    s_laps[0] = elapsed_ms();
    if (s_lap_count < LAP_MAX) {
        s_lap_count++;
    }
    show_laps();
}

static void do_reset(void)
{
    s_running = false;
    s_acc_ms = 0;
    if (s_reset_armed) {
        s_lap_count = 0;
        memset(s_laps, 0, sizeof(s_laps));
        s_reset_armed = false;
        show_laps();
    } else {
        s_reset_armed = true;
    }
    show_time();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_running) {
        show_time();
    }
}

/* ------------------------------------------------------------------ */
/* event handlers                                                      */
/* ------------------------------------------------------------------ */
static void tap_cb(lv_event_t *e)
{
    (void)e;
    do_tap();
}

static void lap_cb(lv_event_t *e)
{
    (void)e;
    do_lap();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    do_reset();
}

/* Screen unload (leaving the app) and delete both tear everything down.
 * The shell creates the next screen before deleting this one, so only the
 * owner may clear the shared state. */
static void screen_gone_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_owner) {
        return;
    }
    s_owner = NULL;
    s_running = false;
    s_acc_ms = 0;
    s_reset_armed = false;
    s_lap_count = 0;
    memset(s_laps, 0, sizeof(s_laps));
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_time_lab = NULL;
    for (int i = 0; i < LAP_MAX; i++) {
        s_lap_lab[i] = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* widgets                                                             */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_lap_label(lv_obj_t *scr, const lv_font_t *font, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(TIMER_ACCENT), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 220);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *scr, const char *text, int x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 88, 88);
    lv_obj_set_pos(b, x, 214);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TIMER_ACCENT), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &hooge55, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_timer_create(int id)
{
    (void)id;

    /* every entry starts a fresh session */
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_running = false;
    s_reset_armed = false;
    s_acc_ms = 0;
    s_lap_count = 0;
    memset(s_laps, 0, sizeof(s_laps));

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(TIMER_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* full-screen tap layer first: the buttons created later sit above it and
     * win the hit test (start / lap / resume). */
    lv_obj_t *tap = lv_button_create(scr);
    lv_obj_remove_style_all(tap);
    lv_obj_set_size(tap, 360, 360);
    lv_obj_set_pos(tap, 0, 0);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(tap, tap_cb, LV_EVENT_CLICKED, NULL);

    /* main time: MM:SS:CC */
    s_time_lab = lv_label_create(scr);
    lv_label_set_text(s_time_lab, "00:00:00");
    lv_obj_set_style_text_font(s_time_lab, &hooge55, 0);
    lv_obj_set_style_text_color(s_time_lab, lv_color_hex(TIMER_ACCENT), 0);
    lv_obj_set_style_text_align(s_time_lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_time_lab, 311);
    lv_obj_align(s_time_lab, LV_ALIGN_TOP_MID, 0, 76);

    /* red separator under the main time */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 279, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_set_style_bg_color(line, lv_color_hex(TIMER_RED), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    /* lap history: newest on top, shrinking sizes like the mockup */
    s_lap_lab[0] = make_lap_label(scr, &hooge36, 154);
    s_lap_lab[1] = make_lap_label(scr, &hooge25, 214);
    s_lap_lab[2] = make_lap_label(scr, &hooge17, 262);

    /* P on the left, R on the right */
    lv_obj_t *bp = make_button(scr, "P", 30);
    lv_obj_t *br = make_button(scr, "R", 243);
    lv_obj_add_event_cb(bp, lap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(br, reset_cb, LV_EVENT_CLICKED, NULL);

    s_owner = scr;
    s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(scr, screen_gone_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(scr, screen_gone_cb, LV_EVENT_DELETE, NULL);

    return scr;
}
