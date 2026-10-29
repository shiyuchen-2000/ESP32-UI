#pragma once

#include <stdbool.h>

/*
 * Process a raw touch event (screen coordinates). Call from the touch read
 * callback BEFORE feeding LVGL.
 *
 * Returns true if the gesture consumed this touch: the caller must then expose
 * LV_INDEV_STATE_RELEASED to LVGL so no button click is generated.
 *
 * Gestures (Android style):
 *   - swipe up from the bottom of the screen  -> desktop (always)
 *   - swipe inward from the left/right edge   -> back one level (desktop at top)
 *   - horizontal swipe on the desktop         -> flip launcher page
 *   - drag up to the middle band and hold     -> multitask (fires on release)
 */
bool gesture_process(bool pressed, int x, int y);
