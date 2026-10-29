#pragma once

#include "lvgl.h"

/* A watch face is a self-contained module:
 *   - build(parent): create the face's widgets inside `parent`, return an
 *     opaque per-instance context (no shared statics, so the main view and the
 *     edit preview can mount instances independently).
 *   - tick(ctx): refresh the time display (called on each second change).
 *   - destroy(ctx): free the context.
 * Add a face by appending one entry to clock_faces[] in clock_faces.c. */
typedef struct {
    const char *name;
    void *(*build)(lv_obj_t *parent);
    void (*tick)(void *ctx);
    void (*destroy)(void *ctx);
} clock_face_t;

extern const clock_face_t clock_faces[];
extern const int clock_face_count;

/* 96px Montserrat (generated, full ASCII) used by the big faces
 * (clock_big_font.c). */
extern const lv_font_t clock_big;

/* hooge 05_55 pixel font at the mockup sizes (hoogeNN.c), used by StarField
 * and by the desktop clock (hooge36, a smaller variant of the same typeface). */
extern const lv_font_t hooge55, hooge36, hooge25, hooge20, hooge17;
