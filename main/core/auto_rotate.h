#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* Start the auto-rotation poller (loads the persisted on/off state from NVS).
 * Call once after the LVGL display exists. */
void auto_rotate_init(lv_display_t *disp);

bool auto_rotate_is_enabled(void);
void auto_rotate_set_enabled(bool on);

/* Current content rotation in degrees (0/90/180/270). */
int  auto_rotate_get_rotation(void);

/* Rotate a physical touch point into logical (content) coordinates. The same
 * transform LVGL itself applies for display rotation, so gesture/click
 * behaviour stays identical to the unrotated case. No-op when rotation == 0. */
void auto_rotate_transform_touch(int *x, int *y);

/* Map a logical rect to its physical (rotated) rect. No-op when rotation == 0. */
void auto_rotate_map_rect(const lv_area_t *in, lv_area_t *out);

/* Rotate a rendered RGB565 strip from logical space into physical space.
 * `dst` must be large enough for the mapped rect (same pixel count). */
void auto_rotate_pixels(const lv_area_t *in, const uint8_t *src,
                        const lv_area_t *out, uint8_t *dst);
