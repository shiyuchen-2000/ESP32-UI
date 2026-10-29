#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_desktop.h"
#include "ui_multitask.h"
#include "wifi_mgr.h"
#include "battery.h"
#include "lvgl.h"
#include <stdio.h>
#include <time.h>

static lv_obj_t *s_cur_scr = NULL;
static lv_obj_t *s_prev_scr = NULL;
static ui_screen_t s_screen = UI_SCREEN_DESKTOP;
static int s_cur_app = -1;

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_batt_label = NULL;
static lv_obj_t *s_batt_fill = NULL; /* proportional fill rect of the battery icon */
static lv_obj_t *s_batt_bolt = NULL; /* charging bolt (shown while charging) */
static uint32_t s_demo_secs = 10 * 3600 + 10 * 60; /* starts at 10:10:00 */

/* Open-apps list (app switcher) */
static int s_open_apps[UI_APP_COUNT];
static int s_open_count = 0;

static void clock_timer_cb(lv_timer_t *t)
{
    /* Real time once ANY source has set the clock: NTP, or the PCF85063 RTC
     * restored at boot (which is what makes WiFi-off reboots work). */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2020) {
        char buf[40];   /* %d on unbounded int can be wide; silence truncation */
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        if (s_time_label) {
            lv_label_set_text(s_time_label, buf);
        }
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        if (s_date_label) {
            lv_label_set_text(s_date_label, buf);
        }
    } else {
        /* demo clock until NTP syncs */
        s_demo_secs++;
        int hh = (s_demo_secs / 3600) % 24;
        int mm = (s_demo_secs / 60) % 60;
        if (s_time_label) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
            lv_label_set_text(s_time_label, buf);
        }
        if (s_date_label) {
            lv_label_set_text(s_date_label, "2026-10-15");
        }
    }

    /* real WiFi status indicator */
    if (s_wifi_label) {
        lv_label_set_text(s_wifi_label, wifi_mgr_is_connected() ? LV_SYMBOL_WIFI : "");
    }

    /* battery level: % text + the icon's proportional fill. While charging, a
     * bolt appears over the icon body and the fill turns green so the charging
     * state is obvious at a glance. */
    if (s_batt_label) {
        int pct = battery_get_percent();
        if (pct >= 0) {
            char bbuf[12];
            snprintf(bbuf, sizeof(bbuf), "%d%%", pct);
            lv_label_set_text(s_batt_label, bbuf);
            bool charging = battery_is_charging();
            if (s_batt_bolt) {
                if (charging) {
                    lv_obj_clear_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_batt_fill) {
                int w = (pct * UI_BATT_FILL_MAXW + 50) / 100;
                if (w > UI_BATT_FILL_MAXW) {
                    w = UI_BATT_FILL_MAXW;
                }
                lv_obj_set_width(s_batt_fill, w);
                uint32_t col = charging ? 0x2F9E44
                             : (pct > 20) ? UI_COLOR_ACCENT
                             : (pct > 10) ? 0xFF9F43
                             : 0xFF3B30;
                lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(col), 0);
            }
        }
    }
}

static void ensure_clock_timer(void)
{
    static lv_timer_t *timer = NULL;
    if (!timer) {
        timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    }
}

void ui_shell_set_clock_labels(lv_obj_t *time_label, lv_obj_t *date_label)
{
    s_time_label = time_label;
    s_date_label = date_label;
}

void ui_shell_set_wifi_label(lv_obj_t *label)
{
    s_wifi_label = label;
}

void ui_shell_set_battery_label(lv_obj_t *label)
{
    s_batt_label = label;
}

void ui_shell_set_battery_fill(lv_obj_t *fill)
{
    s_batt_fill = fill;
}

void ui_shell_set_battery_bolt(lv_obj_t *bolt)
{
    s_batt_bolt = bolt;
}

static void switch_screen(lv_obj_t *new_scr, ui_screen_t type, int cur_app)
{
    /* Delete the screen from TWO navigations ago, never the one we just left:
     * a screen switch can happen while a touch is still in progress (gesture
     * navigation), and deleting that screen would leave the LVGL indev with a
     * dangling object pointer. */
    if (s_prev_scr) {
        lv_obj_delete(s_prev_scr);
        s_prev_scr = NULL;
    }
    s_prev_scr = s_cur_scr;
    s_cur_scr = new_scr;
    s_screen = type;
    s_cur_app = cur_app;
    /* the new screen re-registers its own labels; clear first so a stale
     * pointer is never touched by the clock timer */
    s_time_label = NULL;
    s_date_label = NULL;
    s_wifi_label = NULL;
    s_batt_label = NULL;
    s_batt_fill = NULL;
    s_batt_bolt = NULL;

    lv_screen_load(new_scr);
}

/* ------------------------------------------------------------------ */
/* app lifecycle                                                      */
/* ------------------------------------------------------------------ */
static void open_list_add(int id)
{
    if (id < 0 || id >= UI_APP_COUNT) {
        return;
    }
    for (int i = 0; i < s_open_count; i++) {
        if (s_open_apps[i] == id) {
            return; /* already open */
        }
    }
    if (s_open_count < UI_APP_COUNT) {
        s_open_apps[s_open_count++] = id;
    }
}

static void open_list_remove(int id)
{
    for (int i = 0; i < s_open_count; i++) {
        if (s_open_apps[i] == id) {
            /* shift left */
            for (int j = i; j < s_open_count - 1; j++) {
                s_open_apps[j] = s_open_apps[j + 1];
            }
            s_open_count--;
            return;
        }
    }
}

void ui_shell_open_app(int id)
{
    if (id < 0 || id >= UI_APP_COUNT) {
        return;
    }
    open_list_add(id);
    ensure_clock_timer();
    lv_obj_t *scr = ui_apps[id].create(id);
    switch_screen(scr, UI_SCREEN_APP, id);
}

void ui_shell_close_app(int id)
{
    open_list_remove(id);
    if (s_cur_app == id) {
        /* closing the app that is on screen -> go home */
        ui_shell_home();
    } else if (s_screen == UI_SCREEN_MULTITASK) {
        /* rebuild the switcher without the closed card */
        if (s_open_count == 0) {
            ui_shell_home();
        } else {
            ui_shell_multitask();
        }
    }
}

/* switch_screen clears the saved labels, so after building a fresh desktop we
 * re-point the shell at its labels (clock would otherwise freeze). The clock
 * timer then runs once immediately so the new desktop never shows the
 * placeholder "10:10" / empty indicators for the first second. */
static void repoint_desktop_labels(void)
{
    lv_obj_t *t = NULL, *d = NULL, *w = NULL, *b = NULL, *f = NULL;
    ui_desktop_get_clock_labels(&t, &d);
    ui_shell_set_clock_labels(t, d);
    ui_desktop_get_wifi_label(&w);
    ui_shell_set_wifi_label(w);
    ui_desktop_get_battery_label(&b);
    ui_shell_set_battery_label(b);
    ui_desktop_get_battery_fill(&f);
    ui_shell_set_battery_fill(f);
    lv_obj_t *bolt = NULL;
    ui_desktop_get_battery_bolt(&bolt);
    ui_shell_set_battery_bolt(bolt);
    clock_timer_cb(NULL);   /* fill time / wifi / battery right away */
}

void ui_shell_show_desktop(void)
{
    ensure_clock_timer();
    lv_obj_t *scr = ui_desktop_create();
    switch_screen(scr, UI_SCREEN_DESKTOP, -1);
    repoint_desktop_labels();
}

void ui_shell_desktop_page(int dir)
{
    if (s_screen == UI_SCREEN_DESKTOP) {
        ui_desktop_switch_page(dir);
    }
}

void ui_shell_clear_all(void)
{
    s_open_count = 0;
    ui_shell_home();
}

void ui_shell_home(void)
{
    if (s_screen == UI_SCREEN_DESKTOP) {
        return;
    }
    ui_shell_show_desktop();
}

void ui_shell_back(void)
{
    /* Apps with internal sub-navigation handle back themselves (via the
     * registry hooks); the shell just goes home when they have no inner level. */
    if (s_cur_app >= 0 && ui_apps[s_cur_app].back && ui_apps[s_cur_app].can_back()) {
        ui_apps[s_cur_app].back();
    } else {
        ui_shell_home();
    }
}

void ui_shell_multitask(void)
{
    ensure_clock_timer();
    switch_screen(ui_multitask_create(), UI_SCREEN_MULTITASK, -1);
}

ui_screen_t ui_shell_get_screen(void)
{
    return s_screen;
}

int ui_shell_open_count(void)
{
    return s_open_count;
}

int ui_shell_open_app_at(int index)
{
    if (index < 0 || index >= s_open_count) {
        return -1;
    }
    return s_open_apps[index];
}

bool ui_shell_is_app_open(int id)
{
    for (int i = 0; i < s_open_count; i++) {
        if (s_open_apps[i] == id) {
            return true;
        }
    }
    return false;
}
