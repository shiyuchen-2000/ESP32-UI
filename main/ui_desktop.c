/*
 * Desktop (watch face). All content is laid out inside the visible circle
 * (center 180,180): the status row sits in the top arc, and the shortcut
 * buttons form a ring around the screen edge matching the demo-new/page_1.html
 * wireframe (3 hugging the left edge, 3 the right, middle ones nearest the
 * edge). The bottom gesture band starts below the lowest buttons so tapping
 * an icon never misfires home/multitask. Apps are paged: 6 per page, flipped
 * by a horizontal swipe.
 */
#include "ui_desktop.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "clock_faces.h"   /* hooge pixel fonts for the big clock */
#include "lvgl.h"

/* Ring layout matching demo-new/page_1.html: 6 buttons on a radius-123 ring
 * centered at 180,180 (angles 52/90/128 deg from 12 o'clock), 3 hugging the
 * left edge and 3 the right, symmetric about x=180. The middle buttons sit at
 * the vertical center nearest the edge (~14px from the circle); the upper and
 * lower ones curve inward above and below. The lowest buttons end at y=287,
 * clear of the bottom gesture band (starts at y=292). Positions are TOP-LEFT
 * corners. */
static const int ARC_POS[UI_APP_COUNT][2] = {
    { 53,  73 },  /* left-upper  */
    { 26, 149 },  /* left-mid (center, hugs edge) */
    { 53, 225 },  /* left-lower  */
    {245,  73 },  /* right-upper */
    {272, 149 },  /* right-mid (center, hugs edge) */
    {245, 225 },  /* right-lower */
};

#define BTN_SIZE     62
#define APPS_PER_PAGE 6
#define UI_DESKTOP_MAX_PAGES ((UI_APP_COUNT + APPS_PER_PAGE - 1) / APPS_PER_PAGE)

static int s_page = 0;              /* current launcher page */
static lv_obj_t *s_time_lab = NULL;
static lv_obj_t *s_date_lab = NULL;
static lv_obj_t *s_wifi_lab = NULL;
static lv_obj_t *s_batt_lab = NULL;
static lv_obj_t *s_batt_fill = NULL;  /* proportional fill rect of the battery icon */
static lv_obj_t *s_batt_bolt = NULL;  /* charging bolt shown over the icon body */
static lv_obj_t *s_dots[UI_DESKTOP_MAX_PAGES];    /* page indicator dots (vertical) */

/* pressed feedback: semi-black overlay */
static lv_style_t s_btn_pressed;

static void ensure_pressed_style(void)
{
    static bool init = false;
    if (!init) {
        lv_style_init(&s_btn_pressed);
        lv_style_set_bg_color(&s_btn_pressed, lv_color_hex(0x000000));
        lv_style_set_bg_opa(&s_btn_pressed, LV_OPA_40);
        init = true;
    }
}

static void app_btn_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    ui_shell_open_app(id);
}

static lv_obj_t *make_circle_button(lv_obj_t *parent, int x, int y, uint32_t color, char glyph, int id)
{
    ensure_pressed_style();

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_darken(lv_color_hex(color), 30), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
    lv_obj_add_style(btn, &s_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, app_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    const lv_image_dsc_t *icon = (const lv_image_dsc_t *)ui_apps[id].icon;
    if (icon) {
        lv_obj_t *img = lv_image_create(btn);
        lv_image_set_src(img, icon);
        lv_obj_center(img);
    } else {
        char g[2] = { glyph, '\0' };
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, g);
        lv_obj_set_style_text_color(lab, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(lab, &misans_28, 0);
        lv_obj_center(lab);
    }
    return btn;
}

static void build_status_bar(lv_obj_t *scr, lv_obj_t **out_time, lv_obj_t **out_date)
{
    /* WiFi (left) and battery (right) indicators, leaving the center free for
     * the big clock. Filled by the shell's 1s clock timer. They sit at y=46
     * (not right at the top) so the battery icon + text fits inside the round
     * screen's narrow top arc. */
    s_wifi_lab = lv_label_create(scr);
    lv_label_set_text(s_wifi_lab, "");
    lv_obj_set_style_text_color(s_wifi_lab, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_wifi_lab, &lv_font_montserrat_16, 0);
    lv_obj_align(s_wifi_lab, LV_ALIGN_TOP_MID, -66, 46);

    /* battery: real icon (outline + proportional fill) beside the % text */
    lv_obj_t *batt = lv_obj_create(scr);
    lv_obj_remove_style_all(batt);
    lv_obj_set_size(batt, 66, 16);
    lv_obj_align(batt, LV_ALIGN_TOP_MID, 58, 46);
    lv_obj_clear_flag(batt, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_obj_create(batt);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 22, 11);
    lv_obj_set_pos(body, 0, 2);
    lv_obj_set_style_border_color(body, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_opa(body, LV_OPA_70, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(UI_COLOR_BG_ELEV), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    lv_obj_t *nub = lv_obj_create(batt);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 3, 4);
    lv_obj_set_pos(nub, 22, 5);
    lv_obj_set_style_bg_color(nub, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_70, 0);
    lv_obj_set_style_radius(nub, 1, 0);

    lv_obj_t *fill = lv_obj_create(batt);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, UI_BATT_FILL_MAXW, 7);
    lv_obj_set_pos(fill, 2, 4);
    lv_obj_set_style_bg_color(fill, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 1, 0);
    s_batt_fill = fill;

    s_batt_lab = lv_label_create(batt);
    lv_label_set_text(s_batt_lab, "");
    lv_obj_set_style_text_color(s_batt_lab, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_batt_lab, &hooge17, 0);
    lv_obj_align(s_batt_lab, LV_ALIGN_LEFT_MID, 28, 0);

    /* charging bolt over the icon body; shown by the shell while charging.
     * montserrat (not the pixel font) so the LV_SYMBOL_CHARGE glyph exists. */
    s_batt_bolt = lv_label_create(batt);
    lv_label_set_text(s_batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(s_batt_bolt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_batt_bolt, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_batt_bolt, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);

    /* big clock in the page center, small date below */
    lv_obj_t *time_lab = lv_label_create(scr);
    lv_label_set_text(time_lab, "10:10");
    lv_obj_set_style_text_color(time_lab, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(time_lab, &hooge36, 0);
    lv_obj_align(time_lab, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *date_lab = lv_label_create(scr);
    lv_label_set_text(date_lab, "2026-10-15");
    lv_obj_set_style_text_color(date_lab, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(date_lab, &hooge20, 0);
    lv_obj_align(date_lab, LV_ALIGN_CENTER, 0, 34);

    *out_time = time_lab;
    *out_date = date_lab;
}

/* Small page indicator dots, stacked vertically just right of the left-mid
 * Settings icon (slot 1, ends at x=88). Page 0 is the top dot; page 1 below,
 * matching the up-swipe = next-page direction. Non-interactive; paging is by
 * a vertical swipe in the clock zone. */
static void build_page_dots(lv_obj_t *scr)
{
    int pages = ui_desktop_pages();
    if (pages <= 1) {
        return;
    }
    int y0 = 180 - (pages * 12 - 4) / 2;   /* centered on the Settings icon */
    for (int p = 0; p < pages; p++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 100, y0 + p * 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot,
            (p == s_page) ? lv_color_hex(UI_COLOR_ACCENT) : lv_color_hex(UI_COLOR_FAINT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s_dots[p] = dot;
    }
}

lv_obj_t *ui_desktop_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    build_status_bar(scr, &s_time_lab, &s_date_lab);
    ui_shell_set_clock_labels(s_time_lab, s_date_lab);

    int pages = ui_desktop_pages();
    if (s_page >= pages) {
        s_page = 0;
    }
    int start = s_page * APPS_PER_PAGE;
    int end = start + APPS_PER_PAGE;
    if (end > UI_APP_COUNT) {
        end = UI_APP_COUNT;
    }
    for (int i = start; i < end; i++) {
        int slot = i - start;
        make_circle_button(scr, ARC_POS[slot][0], ARC_POS[slot][1],
                           ui_apps[i].color, ui_apps[i].glyph, i);
    }

    build_page_dots(scr);
    return scr;
}

void ui_desktop_switch_page(int dir)
{
    int pages = ui_desktop_pages();
    if (pages <= 1) {
        return;
    }
    s_page = (s_page + dir + pages) % pages;
    ui_shell_show_desktop(); /* rebuild the desktop for the new page (no animation) */
}

void ui_desktop_get_clock_labels(lv_obj_t **out_time, lv_obj_t **out_date)
{
    *out_time = s_time_lab;
    *out_date = s_date_lab;
}

void ui_desktop_get_wifi_label(lv_obj_t **out_wifi)
{
    *out_wifi = s_wifi_lab;
}

void ui_desktop_get_battery_label(lv_obj_t **out_batt)
{
    *out_batt = s_batt_lab;
}

void ui_desktop_get_battery_fill(lv_obj_t **out_fill)
{
    *out_fill = s_batt_fill;
}

void ui_desktop_get_battery_bolt(lv_obj_t **out_bolt)
{
    *out_bolt = s_batt_bolt;
}

int ui_desktop_pages(void)
{
    return (UI_APP_COUNT + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
}
