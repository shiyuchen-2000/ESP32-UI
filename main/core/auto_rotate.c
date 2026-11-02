/*
 * Auto screen rotation.
 *
 * The watch reads the QMI8658 gravity direction and rotates the whole UI to
 * the nearest of 0/90/180/270 degrees so the content stays upright no matter
 * how the round screen is turned on its handlebar mount.
 *
 * Rendering stays in logical 0-degree space. The LVGL flush callback rotates
 * each flushed strip to its physical position (auto_rotate_map_rect /
 * auto_rotate_pixels), and the touch layer rotates raw points back to logical
 * coordinates (auto_rotate_transform_touch) with exactly the transform LVGL
 * itself uses - so gestures and clicks behave as before, in content space.
 *
 * Toggled from Settings; the on/off state is persisted in NVS. The poll timer
 * always runs to keep the gravity estimate warm, but only applies a rotation
 * while enabled.
 */
#include "auto_rotate.h"
#include "ui_theme.h"
#include "qmi8658.h"
#include "touch_cst816s.h"
#include "nvs.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>

static const char *TAG = "rotate";

#define NVS_NS   "rotate"
#define NVS_ON   "on"

#define POLL_MS    250
#define GRAV_LP    0.20f   /* low-pass of accel -> gravity direction (tau ~1s) */
#define STILL_TOL  0.25f   /* |g| within this of 1G counts as (near-)still */
#define HYST_DEG   25.0f   /* degrees past a sector boundary before rotating */

#define LAST (UI_SCREEN_SIZE - 1)

static qmi8658_dev_t s_imu;
static bool s_imu_ok = false;
static bool s_enabled = false;
static int  s_rotation = 0;    /* 0/90/180/270 */
/* gravity "up", sensor frame. Starts at zero so the detector only begins once
 * the estimate has converged (|g| ~ 1G); starting from {0,0,1} made the screen
 * flip through orientations during the ~1s convergence right after boot. */
static float s_g[3] = { 0.0f, 0.0f, 0.0f };

bool auto_rotate_is_enabled(void)
{
    return s_enabled;
}

int auto_rotate_get_rotation(void)
{
    return s_rotation;
}

/* Re-flush the whole active screen so the new orientation takes effect (the
 * logical content is unchanged, so this only re-runs the flush rotation). */
static void apply_rotation(int rot)
{
    if (rot == s_rotation) {
        return;
    }
    s_rotation = rot;
    ESP_LOGI(TAG, "screen rotation -> %d", rot);
    lv_obj_invalidate(lv_screen_active());
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_ON, s_enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void auto_rotate_set_enabled(bool on)
{
    s_enabled = on;
    nvs_save();
    /* Note: disabling keeps the current orientation (the user asked not to
     * reset it). While off the poller stops applying rotations, so the last
     * orientation simply stays. */
}

static void poll_cb(lv_timer_t *t)
{
    float x, y, z;
    if (!s_imu_ok || qmi8658_read_accel(&s_imu, &x, &y, &z) != ESP_OK) {
        return;
    }
    float a[3] = { x / 1000.0f, y / 1000.0f, z / 1000.0f };  /* read_accel returns mg */
    for (int i = 0; i < 3; i++) {
        s_g[i] += (a[i] - s_g[i]) * GRAV_LP;
    }
    if (!s_enabled) {
        return;
    }

    float gm = sqrtf(s_g[0] * s_g[0] + s_g[1] * s_g[1] + s_g[2] * s_g[2]);
    if (gm < 1e-4f || fabsf(gm - 1.0f) > STILL_TOL) {
        return;   /* accelerating/riding: keep the current rotation */
    }

    /* Direction of "up" (away from gravity) projected onto the screen plane.
     * Sensor +X = screen right, sensor +Y = screen up (verified on this board:
     * left/right auto-rotate was right with +X, and up/down needed +Y treated
     * as screen-up). So the "toward screen down" component is -gd. */
    float gr = s_g[0] / gm;   /* "up" component toward screen right */
    float gd = s_g[1] / gm;   /* "up" component toward screen up (sensor +Y) */

    /* Nearly flat mount (screen facing up): almost no gravity lies in the
     * screen plane, so gr/gd are only sensor noise and the UI flipped between
     * 0 and 180 degrees. Keep the current rotation until the mount is tilted
     * at least ~30 degrees away from horizontal. */
    if (sqrtf(gr * gr + gd * gd) < 0.5f) {
        return;
    }

    float deg = atan2f(-gd, gr) * 180.0f / (float)M_PI;   /* 0=right, 90=down, ... */

    /* Content rotation R makes content-up point at physical angle (270-R).
     * Hysteresis: lock into the current 90-degree sector and only rotate once
     * the angle has moved well past a boundary (HYST_DEG). Without this, a
     * mount sitting near a 45-deg boundary oscillates between two orientations
     * on small noise (seen on the bench: 90 <-> 0 flapping). */
    float center = 270.0f - (float)s_rotation;   /* deg-space center of our sector */
    float d = deg - center;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    int target;
    if (d > 45.0f + HYST_DEG) {
        target = (s_rotation + 270) % 360;
    } else if (d < -(45.0f + HYST_DEG)) {
        target = (s_rotation + 90) % 360;
    } else {
        target = s_rotation;
    }
    if (target != s_rotation) {
        apply_rotation(target);
    }
}

void auto_rotate_transform_touch(int *x, int *y)
{
    int px = *x, py = *y;
    switch (s_rotation) {
        case 90:
            *x = LAST - py;
            *y = px;
            break;
        case 180:
            *x = LAST - px;
            *y = LAST - py;
            break;
        case 270:
            *x = py;
            *y = LAST - px;
            break;
        default:
            break;
    }
}

void auto_rotate_map_rect(const lv_area_t *in, lv_area_t *out)
{
    switch (s_rotation) {
        case 90:
            out->x1 = in->y1;
            out->y1 = LAST - in->x2;
            out->x2 = in->y2;
            out->y2 = LAST - in->x1;
            break;
        case 180:
            out->x1 = LAST - in->x2;
            out->y1 = LAST - in->y2;
            out->x2 = LAST - in->x1;
            out->y2 = LAST - in->y1;
            break;
        case 270:
            out->x1 = LAST - in->y2;
            out->y1 = in->x1;
            out->x2 = LAST - in->y1;
            out->y2 = in->x2;
            break;
        default:
            *out = *in;
            break;
    }
}

void auto_rotate_pixels(const lv_area_t *in, const uint8_t *src,
                        const lv_area_t *out, uint8_t *dst)
{
    int iw = in->x2 - in->x1 + 1;
    int ow = out->x2 - out->x1 + 1;
    for (int ly = in->y1; ly <= in->y2; ly++) {
        for (int lx = in->x1; lx <= in->x2; lx++) {
            int phx, phy;
            switch (s_rotation) {
                case 90:
                    phx = ly;
                    phy = LAST - lx;
                    break;
                case 180:
                    phx = LAST - lx;
                    phy = LAST - ly;
                    break;
                case 270:
                    phx = LAST - ly;
                    phy = lx;
                    break;
                default:
                    phx = lx;
                    phy = ly;
                    break;
            }
            const uint8_t *sp = src + ((ly - in->y1) * iw + (lx - in->x1)) * 2;
            uint8_t *dp = dst + ((phy - out->y1) * ow + (phx - out->x1)) * 2;
            dp[0] = sp[0];
            dp[1] = sp[1];
        }
    }
}

void auto_rotate_init(lv_display_t *disp)
{
    nvs_handle_t h;
    uint8_t on = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_ON, &on);
        nvs_close(h);
    }
    s_enabled = (on != 0);

    s_imu_ok = (qmi8658_init(&s_imu, touch_i2c_bus(), QMI8658_ADDRESS_HIGH) == ESP_OK);
    if (s_imu_ok) {
        qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_2G);
    }
    lv_timer_create(poll_cb, POLL_MS, NULL);
    (void)disp;
}
