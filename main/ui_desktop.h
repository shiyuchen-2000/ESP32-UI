#pragma once

struct _lv_obj_t;

/* Create a new desktop (watch face) screen. Ownership passes to ui_shell. */
struct _lv_obj_t *ui_desktop_create(void);

/* Number of launcher pages (UI_APP_COUNT apps, 6 per page). */
int ui_desktop_pages(void);

/* Move to the next (dir=+1) or previous (dir=-1) launcher page. All pages are
 * pre-rendered inside the desktop; this slides the icon rings vertically. */
void ui_desktop_switch_page(int dir);

/* Max fill width (px) of the desktop battery icon; the shell scales the fill
 * rect to battery_get_percent()/100 of this. */
#define UI_BATT_FILL_MAXW 17

/* Return the desktop's live clock/date labels (the shell re-registers them
 * after a screen switch, since the switch clears the saved pointers). */
void ui_desktop_get_clock_labels(struct _lv_obj_t **out_time, struct _lv_obj_t **out_date);

/* Return the desktop's WiFi status indicator label (same re-registration). */
void ui_desktop_get_wifi_label(struct _lv_obj_t **out_wifi);

/* Return the desktop's battery indicator label (same re-registration). */
void ui_desktop_get_battery_label(struct _lv_obj_t **out_batt);

/* Return the battery icon's fill rect (same re-registration). */
void ui_desktop_get_battery_fill(struct _lv_obj_t **out_fill);

/* Return the charging bolt label (same re-registration). */
void ui_desktop_get_battery_bolt(struct _lv_obj_t **out_bolt);
