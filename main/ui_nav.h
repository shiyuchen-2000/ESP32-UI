#pragma once

#include "lvgl.h"

/* Navigation display app (replaces the Weather placeholder). It shows the
 * state pushed by the MotoNav phone app over BLE. */
lv_obj_t *ui_nav_create(int id);
