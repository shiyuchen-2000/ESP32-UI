/*
 * Screen navigation shell: desktop / app / multitask, with a phone-style
 * app lifecycle (opened apps stay open and appear in the multitask switcher).
 */
#pragma once

#include <stdbool.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

typedef enum {
    UI_SCREEN_DESKTOP,
    UI_SCREEN_APP,
    UI_SCREEN_MULTITASK,
} ui_screen_t;

/* Build and show the desktop (watch face). */
void ui_shell_show_desktop(void);

/* Open app by id (0..UI_APP_COUNT-1); keeps it in the open-apps list. */
void ui_shell_open_app(int id);

/* Close app by id: removes it from the switcher; if it was on screen, go home. */
void ui_shell_close_app(int id);

/* Return to the desktop (no-op if already on it). */
void ui_shell_home(void);

/* Back: if an app shows an inner page (e.g. a Settings detail), go back one
 * level; otherwise go home. Used by the side-edge swipe gesture. */
void ui_shell_back(void);

/* Open the multitask / app-switcher view. */
void ui_shell_multitask(void);

/* Flip the desktop launcher page (+1 next, -1 previous); no-op off desktop.
 * The pages are pre-rendered inside the desktop and slide vertically. */
void ui_shell_desktop_page(int dir);

/* Close every open app and return to the desktop (Android "clear all"). */
void ui_shell_clear_all(void);

ui_screen_t ui_shell_get_screen(void);

/* Open-apps list (for the switcher). */
int ui_shell_open_count(void);
int ui_shell_open_app_at(int index);   /* app id at index, or -1 */
bool ui_shell_is_app_open(int id);

/* Called by screens to hook up the live clock/date labels. */
void ui_shell_set_clock_labels(lv_obj_t *time_label, lv_obj_t *date_label);

/* Hook up the desktop WiFi status indicator label (refreshed by the clock timer). */
void ui_shell_set_wifi_label(lv_obj_t *label);

/* Hook up the desktop battery indicator label (refreshed by the clock timer). */
void ui_shell_set_battery_label(lv_obj_t *label);

/* Hook up the desktop battery icon's fill rect (refreshed by the clock timer). */
void ui_shell_set_battery_fill(lv_obj_t *fill);

/* Hook up the desktop charging bolt label (shown while charging). */
void ui_shell_set_battery_bolt(lv_obj_t *bolt);
