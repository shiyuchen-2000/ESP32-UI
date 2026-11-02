/*
 * Boot splash: black screen, then the W mark fades in, the HONDA wordmark
 * fades in a moment later, and finally a soft white halo flashes once around
 * the whole logo. Ends by calling the user callback (show the desktop).
 *
 * No rotation/transform is used: this board's LVGL software renderer produces
 * a white screen when a rotated object is drawn in partial mode (verified),
 * so the whole animation is plain opacity fades.
 */
#include "boot_anim.h"
#include "logo_mark.h"
#include "logo_honda.h"
#include "logo_glow.h"
#include "lvgl.h"

#define SCR_SIZE        360
#define LOGO_W          204
#define LOGO_H          166
#define LOGO_X          ((SCR_SIZE - LOGO_W) / 2)   /* 78 */
#define LOGO_Y          ((SCR_SIZE - LOGO_H) / 2)   /* 97 */
#define HONDA_Y_OFF     140                          /* wordmark start in the source image */
#define HONDA_H         26

#define MARK_IN_MS      1500
#define HONDA_IN_MS     1200
#define HONDA_DELAY_MS  1000
#define GLOW_IN_MS      600
#define GLOW_OUT_MS     1500

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_glow = NULL;
static void (*s_done)(void) = NULL;

static void opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_image_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void finish_cb(lv_anim_t *a)
{
    (void)a;
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
        s_glow = NULL;
    }
    if (s_done) {
        s_done();
    }
}

static void glow_out_cb(lv_anim_t *a)
{
    (void)a;
    lv_anim_t g;
    lv_anim_init(&g);
    lv_anim_set_var(&g, s_glow);
    lv_anim_set_exec_cb(&g, opa_cb);
    lv_anim_set_values(&g, LV_OPA_COVER, 0);
    lv_anim_set_time(&g, GLOW_OUT_MS);
    lv_anim_set_path_cb(&g, lv_anim_path_linear);
    lv_anim_set_ready_cb(&g, finish_cb);
    lv_anim_start(&g);
}

static void glow_in_cb(lv_anim_t *a)
{
    (void)a;
    lv_anim_t g;
    lv_anim_init(&g);
    lv_anim_set_var(&g, s_glow);
    lv_anim_set_exec_cb(&g, opa_cb);
    lv_anim_set_values(&g, 0, LV_OPA_COVER);
    lv_anim_set_time(&g, GLOW_IN_MS);
    lv_anim_set_path_cb(&g, lv_anim_path_linear);
    lv_anim_set_ready_cb(&g, glow_out_cb);
    lv_anim_start(&g);
}

void boot_anim_start(void (*done)(void))
{
    s_done = done;

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, SCR_SIZE, SCR_SIZE);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* soft halo behind the logo (invisible until the wordmark is in) */
    s_glow = lv_image_create(s_root);
    lv_image_set_src(s_glow, &logo_glow);
    lv_obj_center(s_glow);
    lv_obj_set_style_image_opa(s_glow, 0, 0);

    /* the W mark */
    lv_obj_t *mark = lv_image_create(s_root);
    lv_image_set_src(mark, &logo_mark);
    lv_obj_set_pos(mark, LOGO_X, LOGO_Y);
    lv_obj_set_style_image_opa(mark, 0, 0);

    /* the HONDA wordmark, just below the mark */
    lv_obj_t *honda = lv_image_create(s_root);
    lv_image_set_src(honda, &logo_honda);
    lv_obj_set_pos(honda, LOGO_X, LOGO_Y + HONDA_Y_OFF);
    lv_obj_set_style_image_opa(honda, 0, 0);

    /* stage 1: the W mark fades in */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mark);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_time(&a, MARK_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    /* stage 2: the wordmark fades in slightly later, then the halo flash */
    lv_anim_init(&a);
    lv_anim_set_var(&a, honda);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_time(&a, HONDA_IN_MS);
    lv_anim_set_delay(&a, HONDA_DELAY_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, glow_in_cb);
    lv_anim_start(&a);
}
