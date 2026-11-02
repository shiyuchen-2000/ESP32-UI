/*
 * Multitask / app-switcher (phone style): a vertical stack of cards, one per
 * open app. A big square "x" tab flush with the card's left edge closes that
 * app; tapping the rest of the card opens it. Cards are narrower than before
 * so they sit clear of the screen-edge back-swipe band.
 */
#include "ui_multitask.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"

#define CARD_W     264
#define CARD_H     84
#define CARD_GAP   14
#define CARD_STRIDE (CARD_H + CARD_GAP)

#define LIST_X     ((360 - CARD_W) / 2)
#define LIST_Y     110
#define LIST_H     238

static lv_obj_t *s_list = NULL;

static void card_click_cb(lv_event_t *e)
{
    int app = (int)(intptr_t)lv_event_get_user_data(e);
    ui_shell_open_app(app);
}

/* close the app from its little "x" button; stop bubbling so the card's own
 * CLICKED handler doesn't also open the app */
static void close_btn_cb(lv_event_t *e)
{
    int app = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_stop_bubbling(e);
    ui_shell_close_app(app);
}

static void clear_all_cb(lv_event_t *e)
{
    ui_shell_clear_all();
}

lv_obj_t *ui_multitask_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "多任务");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &misans_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点击卡片打开    x 关闭");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &misans_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 44);

    /* "全部清除" button (Android recents style), only when apps are open */
    if (ui_shell_open_count() > 0) {
        lv_obj_t *clear_btn = lv_button_create(scr);
        lv_obj_set_size(clear_btn, 190, 46);
        lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 0, 62);
        lv_obj_set_style_bg_color(clear_btn, lv_color_hex(UI_COLOR_BG_ELEV), 0);
        lv_obj_set_style_radius(clear_btn, 17, 0);
        lv_obj_set_style_shadow_width(clear_btn, 0, 0);
        lv_obj_set_style_border_width(clear_btn, 0, 0);
        lv_obj_add_event_cb(clear_btn, clear_all_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lab = lv_label_create(clear_btn);
        lv_label_set_text(lab, "全部清除");
        lv_obj_set_style_text_color(lab, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(lab, &misans_16, 0);
        lv_obj_center(lab);
    }

    /* scrollable card list */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, CARD_W, LIST_H);
    lv_obj_set_pos(s_list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    int n = ui_shell_open_count();
    for (int i = 0; i < n; i++) {
        int app = ui_shell_open_app_at(i);

        /* big rounded-rect card */
        lv_obj_t *card = lv_button_create(s_list);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, 0, i * CARD_STRIDE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_BG_ELEV), 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app);

        /* big close tab, flush with the card's left edge (full height, corners
         * matched to the card's radius so it reads as one piece). Solid color:
         * the old vertical gradient showed visible color banding. 92px wide:
         * a gloved finger wants a big target, 64px caused stray taps that
         * opened the app underneath. */
        lv_obj_t *close_btn = lv_button_create(card);
        lv_obj_set_size(close_btn, 92, CARD_H);
        lv_obj_align(close_btn, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(close_btn, 18, 0);
        lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x3A1D24), 0);
        lv_obj_set_style_border_width(close_btn, 0, 0);
        lv_obj_set_style_shadow_width(close_btn, 0, 0);
        lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app);

        lv_obj_t *x = lv_label_create(close_btn);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(x, &lv_font_montserrat_20, 0);
        lv_obj_center(x);

        /* colored accent bar next to the close tab */
        lv_obj_t *accent = lv_obj_create(card);
        lv_obj_set_size(accent, 6, CARD_H - 24);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, 100, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(ui_apps[app].color), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 3, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_pad_all(accent, 0, 0);
        lv_obj_set_scrollbar_mode(accent, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

        /* colored glyph chip */
        lv_obj_t *chip = lv_obj_create(card);
        lv_obj_set_size(chip, 48, 48);
        lv_obj_align(chip, LV_ALIGN_LEFT_MID, 116, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(ui_apps[app].color), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *glyph = lv_label_create(chip);
        char g[2] = { ui_apps[app].glyph, '\0' };
        lv_label_set_text(glyph, g);
        lv_obj_set_style_text_color(glyph, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(glyph, &misans_20, 0);
        lv_obj_center(glyph);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, ui_apps[app].name);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &misans_20, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 176, 0);
    }

    return scr;
}

