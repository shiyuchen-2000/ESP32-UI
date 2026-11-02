/*
 * Placeholder app screen. The user will replace these with real content and
 * SVG icons later; this keeps the navigation framework usable.
 */
#include "ui_app.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"

lv_obj_t *ui_app_create(int id)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    /* title in the top bar */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, ui_apps[id].name);
    lv_obj_set_style_text_color(title, lv_color_hex(ui_apps[id].color), 0);
    lv_obj_set_style_text_font(title, &misans_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    /* divider under the title */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 220, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 46);

    /* big app name (placeholder content) */
    lv_obj_t *big = lv_label_create(scr);
    lv_label_set_text(big, ui_apps[id].name);
    lv_obj_set_style_text_color(big, lv_color_hex(ui_apps[id].color), 0);
    lv_obj_set_style_text_font(big, &misans_28, 0);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_text(note, "内容开发中");
    lv_obj_set_style_text_color(note, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(note, &misans_14, 0);
    lv_obj_align(note, LV_ALIGN_CENTER, 0, 16);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "从底部上滑返回");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &misans_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    return scr;
}
