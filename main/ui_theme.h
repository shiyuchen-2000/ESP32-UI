/*
 * Shared UI constants and the app registry (placeholders until the user
 * provides SVG icons and real app functions).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define UI_APP_COUNT   12
#define UI_APP_SETTINGS 1   /* index of the Settings app */
#define UI_APP_HEART   4   /* index of the Heart app -> G-force meter */
#define UI_SCREEN_SIZE 360

/* Palette */
#define UI_COLOR_BG        0x0A0E14
#define UI_COLOR_BG_ELEV   0x161A22
#define UI_COLOR_TEXT      0xFFFFFF
#define UI_COLOR_DIM       0x8A93A6
#define UI_COLOR_FAINT     0x4B5563
#define UI_COLOR_ACCENT    0x00D1B2

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

/* App registry entry. The shell opens an app purely through these hooks, so
 * adding an app never touches ui_shell / ui_desktop:
 *   - create:  build the app screen; ownership passes to the shell. The app
 *              id is passed (most apps ignore it).
 *   - icon:    optional real icon shown in the desktop circle (NULL = glyph).
 *   - can_back/back: optional internal sub-navigation (e.g. Settings has
 *              detail pages); the shell calls back() when can_back() and goes
 *              home otherwise, without knowing the app's internals. */
typedef struct {
    const char *name;    /* full app name (app screen title) */
    char        glyph;   /* letter shown in the desktop circle when no icon */
    uint32_t    color;   /* accent color */
    const void *icon;    /* const lv_image_dsc_t *, or NULL for the glyph */
    lv_obj_t *(*create)(int id);
    bool (*can_back)(void);
    void (*back)(void);
} ui_app_def_t;

/* The desktop apps (6 per page, paged by swipe). First six are real,
 * the last six are placeholders used to exercise paging. */
extern const ui_app_def_t ui_apps[UI_APP_COUNT];
