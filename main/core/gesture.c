#include "gesture.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "lvgl.h"

/* Bands (360px tall screen) */
#define BOTTOM_Y       292   /* touch must start below this to be a "from bottom" gesture;
                              * the desktop ring's lowest icons end at y=287 */
#define MID_Y_MIN      120
#define MID_Y_MAX      240   /* hold zone for multitask */
#define TOP_Y          100   /* reaching above this + fast = desktop */
#define MOVE_UP        60    /* total upward travel to count as a swipe up */
#define HOLD_MS        200   /* how long to hold still in the middle band */
#define EDGE_W         48    /* left/right edge width that starts a back-swipe */
#define SIDE_SWIPE_DX  24    /* inward travel from the edge to trigger back */
#define PAGE_SWIPE_DY  45    /* vertical travel in the clock zone to flip a page */
/* central clock zone: a swipe starting here flips desktop pages */
#define CLOCK_X_MIN    96
#define CLOCK_X_MAX    264
#define CLOCK_Y_MIN    110
#define CLOCK_Y_MAX    245

static bool s_tracking = false;
static bool s_bottom_started = false;
static bool s_moved_up = false;
static bool s_consumed = false;
static bool s_multitask_pending = false;
static bool s_hold_started = false;
static int s_x0 = 0, s_y0 = 0;
static int s_hold_x = 0, s_hold_y = 0;
static uint32_t s_hold_t0 = 0;
static bool s_side_started = false; /* started in the left/right edge band */
static int  s_side_dir = 0;         /* +1 = left edge (swipe right), -1 = right edge */
static bool s_clock_started = false; /* started in the desktop clock zone (page flip) */

bool gesture_process(bool pressed, int x, int y)
{
    if (!pressed) {
        bool was_consumed = s_consumed;
        if (s_multitask_pending) {
            ui_shell_multitask();
        } else if (s_bottom_started && s_moved_up) {
            /* any swipe up from the bottom always returns to the desktop */
            ui_shell_home();
        }
        s_tracking = false;
        s_bottom_started = false;
        s_moved_up = false;
        s_consumed = false;
        s_multitask_pending = false;
        s_hold_started = false;
        s_side_started = false;
        s_side_dir = 0;
        s_clock_started = false;
        return was_consumed;
    }

    /* pressed */
    if (!s_tracking) {
        s_tracking = true;
        s_bottom_started = false;
        s_moved_up = false;
        s_consumed = false;
        s_multitask_pending = false;
        s_hold_started = false;
        s_x0 = x;
        s_y0 = y;
        s_bottom_started = (y > BOTTOM_Y);
        s_side_started = (x < EDGE_W || x >= UI_SCREEN_SIZE - EDGE_W);
        s_side_dir = (x < EDGE_W) ? 1 : -1;
        s_clock_started = (ui_shell_get_screen() == UI_SCREEN_DESKTOP &&
                           !s_bottom_started && !s_side_started &&
                           x >= CLOCK_X_MIN && x <= CLOCK_X_MAX &&
                           y >= CLOCK_Y_MIN && y <= CLOCK_Y_MAX);
        return false;
    }

    if (s_consumed) {
        /* latch: keep consuming until the finger lifts */
        return true;
    }

    int dy = s_y0 - y; /* positive = moving up */
    int dx = x - s_x0;

    /* A: vertical swipe starting in the central clock zone flips the app
     * pages (up = next page, down = previous). The ring is laid out on the
     * left/right, so paging up/down matches the natural reading order. */
    if (s_clock_started) {
        if (dy > PAGE_SWIPE_DY || dy < -PAGE_SWIPE_DY) {
            ui_shell_desktop_page((dy > 0) ? 1 : -1);
            s_consumed = true;
            return true;
        }
    }

    /* B: swipe inward from the left/right edge -> go back one level
     * (Android style; at the top level this returns to the desktop). The
     * desktop has nothing to go back to, so edge swipes there are ignored. */
    if (s_side_started && ui_shell_get_screen() != UI_SCREEN_DESKTOP) {
        int inward = (s_side_dir > 0) ? dx : -dx;
        if (inward > SIDE_SWIPE_DX) {
            ui_shell_back();
            s_consumed = true;
            return true;
        }
    }

    if (s_bottom_started) {
        if (dy > MOVE_UP) {
            s_moved_up = true;
        }

        if (s_moved_up) {
            /* hold detection in the middle band -> multitask */
            if (y >= MID_Y_MIN && y <= MID_Y_MAX) {
                if (!s_hold_started) {
                    s_hold_started = true;
                    s_hold_x = x;
                    s_hold_y = y;
                    s_hold_t0 = lv_tick_get();
                } else {
                    int dx = x - s_hold_x;
                    int dyy = y - s_hold_y;
                    if (dx * dx + dyy * dyy > 12 * 12) {
                        s_hold_x = x;
                        s_hold_y = y;
                        s_hold_t0 = lv_tick_get();
                    } else if ((uint32_t)(lv_tick_get() - s_hold_t0) > HOLD_MS) {
                        s_multitask_pending = true;
                        s_consumed = true;
                        return true;
                    }
                }
            } else {
                s_hold_started = false;
            }

            /* fast swipe all the way to the top -> desktop */
            if (y < TOP_Y && !s_hold_started) {
                ui_shell_home();
                s_consumed = true;
                return true;
            }
        }
    }

    return s_consumed;
}
