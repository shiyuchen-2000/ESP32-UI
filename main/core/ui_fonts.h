/*
 * System text fonts: MiSans (Chinese + ASCII) at the four sizes the UI uses.
 * Generated with lv_font_conv from DEV/MiSans-Regular.ttf containing only the
 * characters the UI actually shows (ASCII + the Chinese strings in ui_*.c),
 * which keeps each font small enough for flash.
 *
 * The hooge pixel fonts (clock faces, desktop clock) live in clock_faces.h.
 */
#pragma once

#include "lvgl.h"

extern const lv_font_t misans_14, misans_16, misans_20, misans_28;
