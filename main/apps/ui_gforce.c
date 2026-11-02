/*
 * G-force meter (opened from the Heart app icon).
 *
 * The ball shows the *horizontal* inertial force (the rider-felt reaction to
 * braking, acceleration and cornering, i.e. -a), independent of how the watch
 * is mounted. An accelerometer
 * measures specific force: at rest its reading is the 1G gravity vector, split
 * across X/Y/Z by the mount tilt. A tilted handlebar mount pushed most of the
 * real horizontal G onto the Z axis, so the old X/Y-only baseline code barely
 * moved. Here we estimate the gravity direction u during still moments, project
 * it out of every reading (d = a - (a.u)u leaves the horizontal G vector), and
 * map d onto the screen using the sensor's X/Y axes projected onto the
 * horizontal plane.
 *
 * A one-time calibration locks u from a deliberate still moment and persists it
 * to NVS. "Forward" on screen = the horizontal projection of the sensor's
 * screen-up axis (+Y on this board, see the screen mapping below) - the
 * standard handlebar-mount convention (12 o'clock toward the bike's front).
 */
#include "ui_gforce.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "qmi8658.h"
#include "touch_cst816s.h"
#include "auto_rotate.h"
#include "nvs.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>

#define RING_R      160   /* outer ring hugs the screen circle (radius 168) */
#define RING_BORDER 8
#define INNER_R     72    /* thin inner circle ~= 0.6G at PX_PER_G=120 */
#define BALL_R      12
#define PX_PER_G    120.0f /* 1G = 1/3 of the radius; a normal 0.2-0.3G pull
                             * is a clearly visible 24-36px. Pegs (clipped) at
                             * ~1.2G, which is about as hard as a bike gets. */
#define SMOOTH_K     0.22f /* base EMA per 33ms frame on the displayed 2D vector */
#define SMOOTH_K_MIN 0.06f /* strongest damping while the road is jittering */
#define JIT_TAU      0.25f /* low-pass of the frame-to-frame jitter estimate */
#define JIT_GAIN     2.2f  /* how hard a jittery reading pushes K toward the min */
#define DEADZONE_G   0.05f /* soft-pull the ball to centre below this |G| */

#define ACC_LP      0.15f /* low-pass of raw accel = gravity estimate while still */
#define STILL_G_DEV 0.06f /* |a| must be within this of 1G to adapt the gravity dir */
#define STILL_DELTA 0.12f /* per-frame |a - lp| sum below which we count as still */
#define GRAV_FAST   0.12f /* gravity convergence right after the screen opens */
#define GRAV_SLOW   0.01f /* steady-state fine-tuning of the gravity dir (drift) */
#define GRAV_FAST_N 30    /* frames of fast convergence after open */

#define NVS_NS      "gforce"
#define NVS_CAL     "cal"
#define NVS_U       "u"

static const char *TAG = "gforce";

static qmi8658_dev_t s_imu;
static bool s_imu_ok = false;
static lv_obj_t *s_ball = NULL;
static lv_timer_t *s_timer = NULL;
static lv_obj_t *s_timer_owner = NULL; /* screen that currently owns the timer */
static lv_obj_t *s_cal_ov = NULL;      /* calibration overlay (child of the screen) */

static float s_u[3] = { 0.0f, 0.0f, 1.0f }; /* gravity "up" unit vector, sensor frame */
static bool  s_calibrated = false;
static uint32_t s_frames = 0;          /* frames since this screen opened */
static float s_a[3] = { 0, 0, 0 };     /* low-passed accel (= gravity while still) */
static float s_fast[3] = { 0, 0, 0 };  /* ~70ms average: the calibration sample */
static float s_dx = 0.0f, s_dy = 0.0f; /* smoothed horizontal G on the screen axes */
static float s_jit = 0.0f;             /* per-frame jitter estimate (for adaptive K) */
static float s_peak = 0.0f;
static lv_obj_t *s_g_label = NULL;     /* big current-G number */
static lv_obj_t *s_pk_label = NULL;    /* peak-G (session max) */

/* Project sensor axis idx (0/1/2 = X/Y/Z) onto the horizontal plane (the plane
 * perpendicular to the gravity direction u), as a unit vector. Falls back to
 * the next sensor axis if the chosen one is ~parallel to gravity (degenerate
 * mount). */
static void project_axis(const float u[3], int idx, float out[3])
{
    float r[3] = { 0.0f, 0.0f, 0.0f };
    r[idx] = 1.0f;
    float dot = r[0] * u[0] + r[1] * u[1] + r[2] * u[2];
    r[0] -= dot * u[0];
    r[1] -= dot * u[1];
    r[2] -= dot * u[2];
    float n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (n < 1e-4f) {
        int j = (idx + 1) % 3;
        r[0] = r[1] = r[2] = 0.0f;
        r[j] = 1.0f;
        dot = r[0] * u[0] + r[1] * u[1] + r[2] * u[2];
        r[0] -= dot * u[0];
        r[1] -= dot * u[1];
        r[2] -= dot * u[2];
        n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (n < 1e-4f) {
            out[0] = 0.0f;
            out[1] = 1.0f;
            out[2] = 0.0f;
            return;
        }
    }
    float inv = 1.0f / n;
    out[0] = r[0] * inv;
    out[1] = r[1] * inv;
    out[2] = r[2] * inv;
}

/* ------------------------------------------------------------------ */
/* NVS persistence                                                     */
/* ------------------------------------------------------------------ */
static void gforce_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_CAL, 1);
    nvs_set_blob(h, NVS_U, s_u, sizeof(s_u));
    nvs_commit(h);
    nvs_close(h);
}

static bool gforce_nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t cal = 0;
    size_t len = sizeof(s_u);
    bool ok = (nvs_get_u8(h, NVS_CAL, &cal) == ESP_OK && cal &&
               nvs_get_blob(h, NVS_U, s_u, &len) == ESP_OK && len == sizeof(s_u));
    nvs_close(h);
    return ok;
}

/* ------------------------------------------------------------------ */
/* calibration overlay                                                 */
/* ------------------------------------------------------------------ */
static void calib_done_cb(lv_event_t *e)
{
    /* Take the ~70ms average of the *raw* readings as the gravity reference
     * (the user is asked to hold the bike still). Using the slow low-pass here
     * was wrong: it carries values from earlier in the ride, so a tap made
     * shortly after opening the screen could lock in a direction that was not
     * the real "up" - the projection then leaked gravity into the display and
     * the ball sat still (or off-centre) under throttle/brake. Ignore the tap
     * unless the magnitude really is ~1G (readings taken mid-ride are not). */
    float gm = sqrtf(s_fast[0] * s_fast[0] + s_fast[1] * s_fast[1] + s_fast[2] * s_fast[2]);
    if (gm < 0.9f || gm > 1.1f) {
        return;
    }
    float inv = 1.0f / gm;
    s_u[0] = s_fast[0] * inv;
    s_u[1] = s_fast[1] * inv;
    s_u[2] = s_fast[2] * inv;
    s_calibrated = true;
    s_peak = 0.0f;
    s_dx = s_dy = 0.0f;
    gforce_nvs_save();
    if (s_cal_ov) {
        lv_obj_add_flag(s_cal_ov, LV_OBJ_FLAG_HIDDEN); /* child of screen; deleted with it */
        s_cal_ov = NULL;
    }
}

static void calib_skip_cb(lv_event_t *e)
{
    if (s_cal_ov) {
        lv_obj_add_flag(s_cal_ov, LV_OBJ_FLAG_HIDDEN);
        s_cal_ov = NULL;
    }
}

static void show_calib_overlay(lv_obj_t *scr)
{
    lv_obj_t *ov = lv_button_create(scr);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
    lv_obj_add_event_cb(ov, calib_done_cb, LV_EVENT_CLICKED, NULL);
    s_cal_ov = ov;

    lv_obj_t *ti = lv_label_create(ov);
    lv_label_set_text(ti, "G 力校准");
    lv_obj_set_style_text_color(ti, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(ti, &misans_16, 0);
    lv_obj_align(ti, LV_ALIGN_CENTER, 0, -52);

    lv_obj_t *body = lv_label_create(ov);
    lv_label_set_text(body, "保持车辆静止，\n12 点方向朝向正前方，\n然后点击。");
    lv_obj_set_style_text_color(body, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(body, &misans_14, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 240);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *skip = lv_button_create(ov);
    lv_obj_set_style_bg_color(skip, lv_color_hex(UI_COLOR_BG_ELEV), 0);
    lv_obj_set_style_bg_opa(skip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(skip, 14, 0);
    lv_obj_set_size(skip, 84, 34);
    lv_obj_align(skip, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_add_event_cb(skip, calib_skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(skip);
    lv_label_set_text(sl, "跳过");
    lv_obj_set_style_text_color(sl, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(sl, &misans_14, 0);
    lv_obj_center(sl);
}

/* Long-press near the center re-opens the calibration overlay at any time, so
 * calibration is not a one-shot (the auto overlay only appears on first run).
 * Only a press in the central area counts. */
static void recalib_longpress_cb(lv_event_t *e)
{
    if (s_cal_ov) {
        return; /* overlay already up */
    }
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    int dx = (int)pt.x - 180, dy = (int)pt.y - 180;
    if (dx * dx + dy * dy > 120 * 120) {
        return; /* only a press in the central area counts */
    }
    show_calib_overlay(lv_obj_get_parent(lv_event_get_target(e)));
}

/* ------------------------------------------------------------------ */
/* IMU poll timer                                                      */
/* ------------------------------------------------------------------ */
/* The IMU poll timer is a singleton: created once, paused when its screen is
 * deleted, resumed on re-entry. It must NOT be deleted from this callback -
 * ui_gforce_create() runs before switch_screen() deletes the *previous*
 * G-force screen, so a delete here would kill the NEW screen's timer and leak
 * the old one, which then keeps writing to a freed ball every 33ms (UAF ->
 * crash after a few open/close cycles). */
static void screen_delete_cb(lv_event_t *e)
{
    if (s_timer && lv_event_get_target(e) == s_timer_owner) {
        lv_timer_pause(s_timer);
    }
    s_cal_ov = NULL; /* overlay is a child of this screen; drop the stale pointer */
}

static void gforce_timer_cb(lv_timer_t *t)
{
    float x, y, z;
    if (!s_imu_ok || qmi8658_read_accel(&s_imu, &x, &y, &z) != ESP_OK) {
        return;
    }
    float a[3] = { x / 1000.0f, y / 1000.0f, z / 1000.0f }; /* read_accel returns mg */

    /* Seed both estimates from the first frame after the screen opens. They are
     * static across screens, and a leftover value from the previous visit
     * (captured mid-ride) would need ~1s to converge - time the one-tap
     * calibration does not have. */
    if (s_frames == 0) {
        for (int i = 0; i < 3; i++) {
            s_a[i] = a[i];
            s_fast[i] = a[i];
        }
    }

    /* low-pass the raw vector: this tracks gravity when the device is still */
    for (int i = 0; i < 3; i++) {
        s_a[i] += (a[i] - s_a[i]) * ACC_LP;
        s_fast[i] += (a[i] - s_fast[i]) * 0.5f;   /* ~70ms, for calibration */
    }
    float gm = sqrtf(s_a[0] * s_a[0] + s_a[1] * s_a[1] + s_a[2] * s_a[2]);

    /* Once calibrated, u is FROZEN. Adapting it while riding folded the very
     * acceleration we want to show into the gravity baseline: a sustained
     * throttle or brake slowly became "the new down" and the ball barely
     * moved. Re-calibrate with a long-press if the mount changes.
     *
     * Only the never-calibrated case keeps the adaptive estimate: right after
     * the screen opens we converge fast from the {0,0,1} default, afterwards
     * we only fine-tune at a true still moment where the gravity direction
     * already agrees with u (< ~6 deg). */
    if (!s_calibrated) {
        float dev = fabsf(gm - 1.0f);
        float delta = fabsf(a[0] - s_a[0]) + fabsf(a[1] - s_a[1]) + fabsf(a[2] - s_a[2]);
        bool adapt = (dev < STILL_G_DEV && delta < STILL_DELTA);
        if (adapt && s_frames >= GRAV_FAST_N) {
            float dot = (s_a[0] * s_u[0] + s_a[1] * s_u[1] + s_a[2] * s_u[2]) / gm;
            adapt = (dot > 0.995f);   /* gravity dir still aligned with u -> fine-tune */
        }
        if (adapt) {
            float k = (s_frames < GRAV_FAST_N) ? GRAV_FAST : GRAV_SLOW;
            s_u[0] += (s_a[0] - s_u[0] * gm) * k / gm;
            s_u[1] += (s_a[1] - s_u[1] * gm) * k / gm;
            s_u[2] += (s_a[2] - s_u[2] * gm) * k / gm;
            float un = sqrtf(s_u[0] * s_u[0] + s_u[1] * s_u[1] + s_u[2] * s_u[2]);
            if (un > 1e-6f) {
                float iu = 1.0f / un;
                s_u[0] *= iu;
                s_u[1] *= iu;
                s_u[2] *= iu;
            }
        }
    }
    s_frames++;

    /* dynamic (horizontal) G = raw reading minus its gravity component */
    float dot = a[0] * s_u[0] + a[1] * s_u[1] + a[2] * s_u[2];
    float d[3] = { a[0] - dot * s_u[0], a[1] - dot * s_u[1], a[2] - dot * s_u[2] };

    /* Screen axes (both projected onto the horizontal plane, so they rotate
     * with the mount): sensor +X -> screen-right, sensor +Y -> screen-up (the
     * auto-rotate detector confirmed +Y is screen-up on this board). So the
     * ball deflects up toward the "F" label under braking, right on a right
     * turn, etc. */
    float hx[3], hy[3];
    project_axis(s_u, 0, hx);
    project_axis(s_u, 1, hy);

    /* Show the INERTIAL force (-a), not the acceleration: that is what the
     * rider actually feels. Opening the throttle -> d points forward (screen
     * up) -> ball must go BACK/down; braking -> d points back -> ball goes
     * forward/up toward "F". Same flip for cornering: a right turn has d
     * toward the corner (right) and the ball leans left, into the outside. */
    float dx = -(d[0] * hx[0] + d[1] * hx[1] + d[2] * hx[2]);   /* screen right */
    float dy = -(d[0] * hy[0] + d[1] * hy[1] + d[2] * hy[2]);   /* screen up */

    /* Adaptive smoothing: on a bumpy street the frame-to-frame movement of the
     * raw vector stays large every frame (engine/road vibration), on a clean
     * road it is the real G signal. Estimate that jitter and use it to lower
     * the EMA gain while the road is rough, so the ball stops shaking but still
     * reacts to real braking/cornering (a real event only produces one big
     * step, then the jitter estimate decays). */
    float delta = fabsf(dx - s_dx) + fabsf(dy - s_dy);
    s_jit += (delta - s_jit) * JIT_TAU;
    float k = SMOOTH_K * (1.0f - JIT_GAIN * s_jit);
    if (k < SMOOTH_K_MIN) {
        k = SMOOTH_K_MIN;
    }
    s_dx += (dx - s_dx) * k;
    s_dy += (dy - s_dy) * k;

    /* Dead zone: soft-pull the ball to the centre near zero so it stops
     * crawling around from sensor noise / idling vibration when parked. */
    float g = sqrtf(s_dx * s_dx + s_dy * s_dy);
    if (g < DEADZONE_G) {
        float t = g * (1.0f / DEADZONE_G);   /* 0..1 */
        float sc = t * t;                    /* strong pull at centre, smooth at edge */
        s_dx *= sc;
        s_dy *= sc;
        g *= sc;
    }

    float px = s_dx * PX_PER_G, py = s_dy * PX_PER_G;
    float dist = sqrtf(px * px + py * py);
    float maxr = (float)(RING_R - BALL_R - 4);
    if (dist > maxr && dist > 1e-6f) {
        px *= maxr / dist;
        py *= maxr / dist;
    }
    /* Ball centre in PHYSICAL screen coords (+px right, +py up). Map it to
     * logical space with the same physical->logical rotation the touch layer
     * uses, so the auto-rotate flush renders the ball at the correct physical
     * direction. No-op when rotation == 0; without this the ball deflected
     * relative to the sensor axes while the rest of the UI rotated. */
    int bx = (int)(180.0f + px);
    int by = (int)(180.0f - py);
    auto_rotate_transform_touch(&bx, &by);
    lv_obj_set_pos(s_ball, bx - BALL_R, by - BALL_R);

    if (g > s_peak) {
        s_peak = g;
    }
    if (s_g_label) {
        char b[16];
        snprintf(b, sizeof(b), "%.1fG", g);
        lv_label_set_text(s_g_label, b);
    }
    if (s_pk_label) {
        char b[24];
        snprintf(b, sizeof(b), "峰值 %.1fG", s_peak);
        lv_label_set_text(s_pk_label, b);
    }

    /* One line every ~2s on the console: raw accel, the horizontal vector the
     * ball uses, and where u points. If the ball "does not move", this says
     * exactly which stage is flat (sensor, projection, or display). */
    if ((s_frames % 60) == 0) {
        ESP_LOGI(TAG, "a=(%.2f %.2f %.2f) d=(%.2f %.2f %.2f) xy=(%.2f %.2f) G=%.2f jit=%.2f k=%.2f",
                 a[0], a[1], a[2], d[0], d[1], d[2], s_dx, s_dy, g, s_jit, k);
    }
}

lv_obj_t *ui_gforce_create(int id)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    (void)id;
    s_frames = 0;
    s_peak = 0.0f;
    s_dx = s_dy = 0.0f;
    s_jit = 0.0f;
    s_calibrated = gforce_nvs_load(); /* restore a saved calibration, if any */

    if (!s_imu_ok) {
        s_imu_ok = (qmi8658_init(&s_imu, touch_i2c_bus(), QMI8658_ADDRESS_HIGH) == ESP_OK);
        if (s_imu_ok) {
            /* ±2g gives finer resolution (less relative noise) for a 0-2G meter */
            qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_2G);
        }
    }

    /* red outer ring, hugging the screen edge and drawn thick */
    lv_obj_t *ring = lv_obj_create(scr);
    lv_obj_set_size(ring, RING_R * 2, RING_R * 2);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, 0, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_border_width(ring, RING_BORDER, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    /* thin inner circle (1G) */
    lv_obj_t *inner = lv_obj_create(scr);
    lv_obj_set_size(inner, INNER_R * 2, INNER_R * 2);
    lv_obj_center(inner);
    lv_obj_set_style_bg_opa(inner, 0, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(0xFF7F00), 0);
    lv_obj_set_style_border_width(inner, 2, 0);
    lv_obj_set_style_border_opa(inner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);

    /* crosshair spanning most of the ring */
    lv_obj_t *h = lv_obj_create(scr);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, 296, 2);
    lv_obj_set_pos(h, 32, 179);
    lv_obj_set_style_bg_color(h, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_50, 0);

    lv_obj_t *v = lv_obj_create(scr);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, 2, 296);
    lv_obj_set_pos(v, 179, 32);
    lv_obj_set_style_bg_color(v, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_50, 0);

    /* thick dark-gray segmented decoration (4 arcs) just inside the outer
     * ring; 72deg segments with 18deg gaps centered on the crosshair axes
     * (0/90/180/270), square ends, kept inside the red ring (band 134..154) */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *seg = lv_arc_create(scr);
        lv_obj_set_size(seg, 288, 288);   /* radius 144 */
        lv_obj_center(seg);
        lv_arc_set_rotation(seg, i * 90);
        lv_arc_set_bg_angles(seg, 9, 81);   /* 72deg segment, 18deg gap on the axis */
        lv_arc_set_angles(seg, 0, 0);
        lv_obj_remove_style(seg, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_width(seg, 20, LV_PART_MAIN);
        lv_obj_set_style_arc_color(seg, lv_color_hex(UI_COLOR_FAINT), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(seg, false, LV_PART_MAIN);
    }

    /* direction hints: F = forward, B = backward, L/R = left/right. "Forward"
     * is the horizontal projection of the sensor's screen-up axis, so it is
     * only meaningful after the one-time calibration. Placed in the 18 deg
     * gaps the arcs leave at the four cardinal axes (radius ~145). */
    lv_obj_t *lab_f = lv_label_create(scr);
    lv_label_set_text(lab_f, "前");
    lv_obj_set_style_text_color(lab_f, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(lab_f, &misans_14, 0);
    lv_obj_align(lab_f, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *lab_b = lv_label_create(scr);
    lv_label_set_text(lab_b, "后");
    lv_obj_set_style_text_color(lab_b, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(lab_b, &misans_14, 0);
    lv_obj_align(lab_b, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_t *lab_l = lv_label_create(scr);
    lv_label_set_text(lab_l, "左");
    lv_obj_set_style_text_color(lab_l, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(lab_l, &misans_14, 0);
    lv_obj_align(lab_l, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *lab_r = lv_label_create(scr);
    lv_label_set_text(lab_r, "右");
    lv_obj_set_style_text_color(lab_r, lv_color_hex(UI_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(lab_r, &misans_14, 0);
    lv_obj_align(lab_r, LV_ALIGN_RIGHT_MID, -30, 0);

    /* current-G readout: dark pill ABOVE the orange 1G circle (its top edge is
     * at y=108), so the number never sits on the center where the ball lives */
    lv_obj_t *pill = lv_obj_create(scr);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 116, 40);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_COLOR_BG_ELEV), 0);
    lv_obj_set_style_bg_opa(pill, 0xD0, 0);
    lv_obj_set_style_radius(pill, 18, 0);

    s_g_label = lv_label_create(pill);
    lv_label_set_text(s_g_label, "0.00G");
    lv_obj_set_style_text_color(s_g_label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_g_label, &misans_28, 0);
    lv_obj_center(s_g_label);

    /* peak-G (session max; resets each time the app opens) */
    s_pk_label = lv_label_create(scr);
    lv_label_set_text(s_pk_label, "峰值 0.00G");
    lv_obj_set_style_text_color(s_pk_label, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_pk_label, &misans_14, 0);
    lv_obj_align(s_pk_label, LV_ALIGN_BOTTOM_MID, 0, -88);

    /* white ball, starts centered. Created after the pill so it draws on top;
     * not clickable so a press in the center reaches the long-press layer. */
    lv_obj_t *ball = lv_obj_create(scr);
    lv_obj_set_size(ball, BALL_R * 2, BALL_R * 2);
    lv_obj_set_pos(ball, 180 - BALL_R, 180 - BALL_R);
    lv_obj_set_style_bg_color(ball, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(ball, 8, 0);
    lv_obj_set_style_shadow_color(ball, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(ball, LV_OPA_50, 0);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_CLICKABLE);
    s_ball = ball;

    /* transparent long-press layer on top: long-press near the center re-opens
     * the calibration overlay (see recalib_longpress_cb) */
    lv_obj_t *lp = lv_button_create(scr);
    lv_obj_remove_style_all(lp);
    lv_obj_set_size(lp, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(lp, 0, 0);
    lv_obj_set_style_bg_opa(lp, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(lp, recalib_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* animate the ball with a persistent singleton timer */
    s_timer_owner = scr;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(gforce_timer_cb, 33, NULL);
    } else {
        lv_timer_reset(s_timer);
        lv_timer_resume(s_timer);
    }

    /* pause the timer only when THIS screen is destroyed */
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);

    /* first run (or after a wipe): ask for the one-time calibration */
    if (!s_calibrated) {
        show_calib_overlay(scr);
    }

    return scr;
}
