#include "ui_theme.h"
#include "ui_icons.h"
#include "ui_app.h"
#include "ui_settings.h"
#include "ui_gforce.h"
#include "ui_clock.h"
#include "ui_nav.h"
#include "ui_timer.h"
#include "ui_music.h"

/* The registry is the single place that maps app ids to their screens. The
 * shell opens apps through ui_apps[id].create() and navigates back through
 * can_back()/back(), so it never needs to know any app's internals. */
const ui_app_def_t ui_apps[UI_APP_COUNT] = {
    { .name = "时钟",   .glyph = 'C', .color = 0x00D1B2, .icon = &s_icon_clock,
      .create = ui_clock_create,
      .can_back = ui_clock_can_back, .back = ui_clock_back },
    { .name = "设置",   .glyph = 'S', .color = 0x2B6CB0, .icon = &s_icon_settings,
      .create = ui_settings_create, .can_back = ui_settings_can_back, .back = ui_settings_do_back },
    { .name = "音乐",   .glyph = 'Q', .color = 0x9C36B5, .icon = &s_icon_music,
      .create = ui_music_create },
    { .name = "导航",   .glyph = 'N', .color = 0x00A6FF, .icon = &s_icon_nav,
      .create = ui_nav_create },
    { .name = "G力",    .glyph = 'G', .color = 0xE8590C, .icon = &s_icon_gforce,
      .create = ui_gforce_create },
    { .name = "计时",   .glyph = 'T', .color = 0x7950F2, .icon = &s_icon_timer,
      .create = ui_timer_create },
    /* page-2 placeholders (exercise the paging UI; replace later) */
    { .name = "地图",   .glyph = 'M', .color = 0x2E9E5B, .create = ui_app_create },
    { .name = "音乐",   .glyph = 'Q', .color = 0x9C36B5, .create = ui_app_create },
    { .name = "电话",   .glyph = 'P', .color = 0x339AF0, .create = ui_app_create },
    { .name = "相机",   .glyph = 'C', .color = 0xE67E22, .create = ui_app_create },
    { .name = "备忘录", .glyph = 'N', .color = 0x20C997, .create = ui_app_create },
    { .name = "日历",   .glyph = 'D', .color = 0xE03131, .create = ui_app_create },
};
