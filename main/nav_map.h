/*
 * Vector map renderer for the MotoNav protocol (first map stage).
 *
 * The phone sends two bulk blobs:
 *   type 1  map chunk: u16 frame, i32 ox, i32 oy, u16 n,
 *           n × { u8 class (bit7 = polygon), u16 point count,
 *                 count × (i16 x, i16 y) }   (metres, relative to ox/oy)
 *   type 2  route:     u16 frame, i32 x0, i32 y0, u16 n,
 *                      (n-1) × (i16 dx, i16 dy), then optional traffic lights
 *
 * Rendering happens straight into an RGB565 canvas buffer in PSRAM. The
 * palette follows the phone's #t (style) and #n (night) commands, so the
 * app's style picker is reused as-is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "nav_proto.h"

void nav_map_init(void);

/* Takes ownership of buf (PSRAM) and replaces that blob type. */
void nav_map_set_blob(int type, uint8_t *buf, uint32_t len);

/* True when a map chunk (type 1) is available for the current frame. */
bool nav_map_has_map(void);

/* Changes whenever the map needs a redraw because a blob was replaced. */
uint32_t nav_map_version(void);

/* Draw the map into an RGB565 buffer. Returns false when there is nothing to
 * draw (no map / map switched off). */
bool nav_map_render(uint16_t *dst, int w, int h, const nav_state_t *st);
