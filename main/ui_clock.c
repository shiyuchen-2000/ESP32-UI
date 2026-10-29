/*
 * Clock app: an immersive, switchable watch-face screen.
 *
 * Fully decoupled: it depends only on LVGL, NVS and the standard time
 * functions (no shell/desktop/gesture/app knowledge). The faces live in
 * clock_faces.c behind a tiny registry; adding a face is one registry entry.
 *
 * Interaction:
 *   - long-press the face  -> open the face editor
 *   - editor: swipe left/right changes the face; "Done" saves the selection
 *     (NVS) and returns; edge-swipe back cancels (no save).
 */
#include "ui_clock.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "clock_faces.h"
#include "nvs.h"
#include "lvgl.h"
#include <time.h>

#define NVS_NS   "clock"
#define NVS_FACE "face"

#define SWIPE_DX   45   /* horizontal drag (px) that changes the face */
#define MAX_FACES  8    /* dots indicator capacity */
#define EDIT_TITLE "选择表盘"

static lv_obj_t *s_scr_root = NULL;   /* the app screen (timer owner) */
static lv_obj_t *s_main_host = NULL;  /* normal-view host for the face */
static int       s_face_idx = 0;      /* persisted selection */
static bool      s_in_edit = false;
static lv_obj_t *s_edit = NULL;       /* editor overlay */
static lv_obj_t *s_preview_host = NULL;
static int       s_edit_idx = 0;      /* editor selection */
static lv_obj_t *s_dots[MAX_FACES];
static int s_edit_x0 = 0, s_edit_y0 = 0;   /* editor drag start */

/* the single currently-mounted face (normal view or editor preview) */
static lv_obj_t *s_host = NULL;
static int       s_mount_idx = 0;
static void     *s_mount_ctx = NULL;
static lv_obj_t *s_mount_owner = NULL;   /* screen that owns the mounted face */

static lv_timer_t *s_timer = NULL;
static lv_obj_t   *s_timer_owner = NULL;
static int        s_last_sec = -1;

static void clock_nvs_save(void);
static void close_edit(bool save);

/* ------------------------------------------------------------------ */
/* face mounting                                                       */
/* ------------------------------------------------------------------ */
/* Tear down whatever face is currently mounted (clean its widgets + free its
 * context). Exactly one face is mounted at a time (normal view or editor
 * preview), so nothing ever stacks on top of an old instance. */
static void unmount_face(void)
{
    if (s_host) {
        lv_obj_clean(s_host);
    }
    if (s_mount_ctx) {
        clock_faces[s_mount_idx].destroy(s_mount_ctx);
    }
    s_mount_ctx = NULL;
    s_host = NULL;
}

/* Faces are purely visual: clear CLICKABLE/SCROLLABLE on every widget so they
 * never intercept the editor's swipe layer (or the long-press layer). */
static void clear_interactive(lv_obj_t *host)
{
    uint32_t n = lv_obj_get_child_count(host);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(host, i);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        clear_interactive(c);
    }
}

static void mount_into(lv_obj_t *host, int idx)
{
    unmount_face();
    s_mount_ctx = clock_faces[idx].build(host);
    clear_interactive(host);
    s_mount_idx = idx;
    s_host = host;
}

/* ------------------------------------------------------------------ */
/* 1s tick (singleton + owner-pause, like the other screen timers)     */
/* ------------------------------------------------------------------ */
static void tick_cb(lv_timer_t *t)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_sec != s_last_sec) {
        s_last_sec = tm.tm_sec;
        if (s_mount_ctx) {
            clock_faces[s_mount_idx].tick(s_mount_ctx);
        }
    }
}

static void screen_delete_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    if (s_timer && t == s_timer_owner) {
        lv_timer_pause(s_timer);
    }
    /* Only tear down shared state if THIS screen owns it. The shell deletes the
     * PREVIOUS screen on the next switch, which can be an old Clock screen while
     * a new one is already mounted - without the owner check that stale delete
     * would free the NEW face's context (UAF) and the time would freeze at
     * "--:--" after re-entering the app. */
    if (t == s_mount_owner) {
        if (s_mount_ctx) {
            clock_faces[s_mount_idx].destroy(s_mount_ctx);
        }
        s_mount_ctx = NULL;
        s_host = NULL;
        s_edit = NULL;
        s_scr_root = NULL;
        s_in_edit = false;
    }
}

/* ------------------------------------------------------------------ */
/* persistence                                                         */
/* ------------------------------------------------------------------ */
static int clock_nvs_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_FACE, &v);
        nvs_close(h);
    }
    return v;
}

static void clock_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_FACE, (uint8_t)s_face_idx);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* face editor                                                         */
/* ------------------------------------------------------------------ */
static void update_dots(void)
{
    for (int i = 0; i < clock_face_count && i < MAX_FACES; i++) {
        if (s_dots[i]) {
            lv_obj_set_style_bg_color(s_dots[i],
                lv_color_hex(i == s_edit_idx ? UI_COLOR_ACCENT : UI_COLOR_FAINT), 0);
        }
    }
}

static void set_preview(int idx)
{
    unmount_face();   /* also tears down the normal face when the editor opens */
    s_mount_ctx = clock_faces[idx].build(s_preview_host);
    clear_interactive(s_preview_host);
    s_mount_idx = idx;
    s_edit_idx = idx;   /* keep the editor selection in sync (Done saves this!) */
    s_host = s_preview_host;
    update_dots();
}

static void edit_swipe_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_edit_x0 = pt.x;
        s_edit_y0 = pt.y;
    } else if (lv_event_get_code(e) == LV_EVENT_RELEASED ||
               lv_event_get_code(e) == LV_EVENT_PRESS_LOST) {
        int dx = pt.x - s_edit_x0;
        int dy = pt.y - s_edit_y0;
        int ax = dx < 0 ? -dx : dx;
        int ay = dy < 0 ? -dy : dy;
        if (ax > SWIPE_DX && ax > 2 * ay) {
            if (dx > 0) {
                set_preview((s_edit_idx + clock_face_count - 1) % clock_face_count);
            } else {
                set_preview((s_edit_idx + 1) % clock_face_count);
            }
        }
    }
}

static void done_cb(lv_event_t *e)
{
    close_edit(true);
}

static void open_edit(void)
{
    if (s_in_edit) {
        return;
    }
    s_in_edit = true;
    s_edit_idx = s_mount_idx;

    lv_obj_t *ov = lv_obj_create(s_scr_root);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_edit = ov;

    /* preview area (the face itself, full size) */
    s_preview_host = lv_obj_create(ov);
    lv_obj_set_size(s_preview_host, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(s_preview_host, 0, 0);
    lv_obj_set_style_bg_opa(s_preview_host, 0, 0);
    lv_obj_set_style_border_width(s_preview_host, 0, 0);
    lv_obj_set_style_pad_all(s_preview_host, 0, 0);
    lv_obj_clear_flag(s_preview_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_preview_host, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ti = lv_label_create(ov);
    lv_label_set_text(ti, EDIT_TITLE);
    lv_obj_set_style_text_color(ti, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(ti, &misans_16, 0);
    lv_obj_align(ti, LV_ALIGN_TOP_MID, 0, 12);

    /* page dots */
    for (int i = 0; i < clock_face_count && i < MAX_FACES; i++) {
        lv_obj_t *d = lv_obj_create(ov);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - (clock_face_count - 1) / 2) * 20, -70);
        s_dots[i] = d;
    }

    /* mount the preview face first, THEN the transparent swipe layer so it sits
     * on top of the face (with face widgets non-interactive, the swipe is never
     * stolen). Title/dots stay above; Done is created last, above everything. */
    set_preview(s_edit_idx);

    lv_obj_t *sw = lv_button_create(ov);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(sw, 0, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(sw, edit_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sw, edit_swipe_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(sw, edit_swipe_cb, LV_EVENT_PRESS_LOST, NULL);

    /* save button */
    lv_obj_t *btn = lv_button_create(ov);
    lv_obj_set_size(btn, 96, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_add_event_cb(btn, done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "完成");
    lv_obj_set_style_text_color(l, lv_color_hex(0x06231B), 0);
    lv_obj_set_style_text_font(l, &misans_20, 0);
    lv_obj_center(l);
}

static void close_edit(bool save)
{
    if (!s_in_edit) {
        return;
    }
    if (save) {
        s_face_idx = s_edit_idx;
        clock_nvs_save();
    }
    /* tear down the editor preview face */
    unmount_face();
    if (s_edit) {
        lv_obj_delete(s_edit);
        s_edit = NULL;
    }
    s_in_edit = false;
    /* remount the normal face (s_main_host was cleaned when the editor opened) */
    mount_into(s_main_host, s_face_idx);
}

/* ------------------------------------------------------------------ */
/* shell hooks                                                         */
/* ------------------------------------------------------------------ */
bool ui_clock_can_back(void)
{
    return s_in_edit;
}

void ui_clock_back(void)
{
    close_edit(false);
}

/* ------------------------------------------------------------------ */
/* create                                                              */
/* ------------------------------------------------------------------ */
static void longpress_cb(lv_event_t *e)
{
    open_edit();
}

lv_obj_t *ui_clock_create(int id)
{
    (void)id;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);

    s_scr_root = scr;
    s_in_edit = false;
    s_edit = NULL;
    s_last_sec = -1;
    /* NOTE: don't NULL s_host/s_mount_ctx here - mount_into()'s unmount_face()
     * tears down whatever the PREVIOUS screen still has mounted (avoids
     * leaking its face context on re-entry). */

    s_face_idx = clock_nvs_load();
    if (s_face_idx < 0 || s_face_idx >= clock_face_count) {
        s_face_idx = 0;
    }

    /* normal face view */
    s_main_host = lv_obj_create(scr);
    lv_obj_set_size(s_main_host, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(s_main_host, 0, 0);
    lv_obj_set_style_bg_opa(s_main_host, 0, 0);
    lv_obj_set_style_border_width(s_main_host, 0, 0);
    lv_obj_set_style_pad_all(s_main_host, 0, 0);   /* content origin == 0,0 so lv_line absolute coords align */
    lv_obj_clear_flag(s_main_host, LV_OBJ_FLAG_SCROLLABLE);
    mount_into(s_main_host, s_face_idx);
    s_mount_owner = scr;

    /* long-press layer: opens the face editor */
    lv_obj_t *lp = lv_button_create(scr);
    lv_obj_remove_style_all(lp);
    lv_obj_set_size(lp, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(lp, 0, 0);
    lv_obj_set_style_bg_opa(lp, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(lp, longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* 1s tick (250ms timer, updates on the second) */
    s_timer_owner = scr;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(tick_cb, 250, NULL);
    } else {
        lv_timer_reset(s_timer);
        lv_timer_resume(s_timer);
    }

    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}
