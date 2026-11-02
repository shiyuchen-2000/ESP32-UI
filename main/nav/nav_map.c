#include "nav_map.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nav_map";

/* Declared before the raster block; defined further down with the rest of the
 * drawing helpers. */
static inline uint16_t rd16(const uint8_t *p);
static inline int16_t  rds16(const uint8_t *p);
static inline int32_t  rd32(const uint8_t *p);

typedef struct map_theme_s {
    uint16_t block, road, park, water, route, route2, hud, blip, done, done2;
} map_theme_t;

static int       W, H;
static uint16_t *DST;
static float     s_sc, s_inv_sc, s_cs, s_sn, s_posx, s_posy;
static int       s_cx, s_py;

/* Blend c0 toward c1 by weight w (0..255). Uses a /255 approximation
 * (×257>>16) so the per-pixel building-roof path stays cheap. */
static inline uint16_t blend565(uint16_t c0, uint16_t c1, int w)
{
    int r = ((int)((c0 >> 11) & 0x1F) * (255 - w) + (int)((c1 >> 11) & 0x1F) * w) * 257 >> 16;
    int g = ((int)((c0 >> 5) & 0x3F) * (255 - w) + (int)((c1 >> 5) & 0x3F) * w) * 257 >> 16;
    int b = ((int)(c0 & 0x1F) * (255 - w) + (int)(c1 & 0x1F) * w) * 257 >> 16;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ------------------------------------------------------------------ */
/* blobs (owned PSRAM buffers)                                        */
/* ------------------------------------------------------------------ */
#define BLOB_TYPES 5
static uint8_t  *s_buf[BLOB_TYPES];
static uint32_t  s_len[BLOB_TYPES];
static uint8_t  *s_old[BLOB_TYPES];   /* one generation back, freed on next set */
static uint32_t  s_gen[BLOB_TYPES];   /* bumped on every new blob */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void nav_map_init(void)
{
    for (int i = 0; i < BLOB_TYPES; i++) {
        s_buf[i] = s_old[i] = NULL;
        s_len[i] = 0;
        s_gen[i] = 0;
    }
}

void nav_map_set_blob(int type, uint8_t *buf, uint32_t len)
{
    if (type <= 0 || type >= BLOB_TYPES) {
        free(buf);
        return;
    }
    uint8_t *to_free;
    taskENTER_CRITICAL(&s_lock);
    to_free = s_old[type];     /* two generations old: a render cannot still use it */
    s_old[type] = s_buf[type];
    s_buf[type] = buf;
    s_len[type] = len;
    s_gen[type]++;
    taskEXIT_CRITICAL(&s_lock);
    free(to_free);             /* heap calls must stay outside the critical section */
    ESP_LOGI(TAG, "blob type=%d len=%u", type, (unsigned)len);
}

/* Cheap signature of the blob set: any new map / route / raster / ghost blob
 * bumps it, so the UI can skip re-rendering an unchanged frame. */
uint32_t nav_map_version(void)
{
    uint32_t v;
    taskENTER_CRITICAL(&s_lock);
    v = (s_gen[1] * 31u) ^ (s_gen[2] * 131u) ^ (s_gen[3] * 8191u) ^ (s_gen[4] * 131071u);
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

bool nav_map_has_map(void)
{
    bool ok;
    taskENTER_CRITICAL(&s_lock);
    ok = (s_buf[1] != NULL && s_len[1] >= 12) ||
         (s_buf[3] != NULL && s_len[3] >= 16);
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* raster basemap (Amap screenshot / Tianditu), 4bpp with 16x16 blocks */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     valid;
    uint32_t gen;
    int      w, h, bw;
    int32_t  x0, y0;
    float    mpp;
    float    inv_mpp;   /* 1/mpp: the per-pixel sampling does not divide */
    bool     pal;
    bool     bld;       /* 0x4000: extra 4bpp colours 4..7 are roofs/outlines */
    uint16_t pal16[16];
} ras_info_t;

static ras_info_t s_ras;
static uint8_t   *s_ras_buf;
static uint32_t   s_ras_cap;

static bool raster_decode(const uint8_t *p, uint32_t len, uint32_t gen)
{
    if (len < 16) {
        return false;
    }
    int32_t x0 = rd32(p + 2), y0 = rd32(p + 6);
    int w = rd16(p + 10), h = rd16(p + 12);
    uint16_t flags = rd16(p + 14);
    bool pal = (flags & 0x8000) != 0;
    bool bld = (flags & 0x4000) != 0;
    int mm = flags & 0x3FFF;
    if (!w || !h || !mm || w > 2048 || h > 2048) {
        return false;
    }
    int bw = (w + 15) / 16;
    uint32_t bytes = (uint32_t)bw * ((h + 15) / 16) * 128;
    if (bytes > s_ras_cap) {
        uint8_t *nb = heap_caps_realloc(s_ras_buf, bytes, MALLOC_CAP_SPIRAM);
        if (!nb) {
            ESP_LOGE(TAG, "no PSRAM for raster %u bytes", (unsigned)bytes);
            return false;
        }
        s_ras_buf = nb;
        s_ras_cap = bytes;
    }
    memset(s_ras_buf, 0, bytes);

    uint32_t pos = pal ? 48 : 16;
    if (pos > len) {
        return false;
    }
    uint32_t total = (uint32_t)w * h, done = 0;
    int x = 0, y = 0;
    while (pos < len && done < total) {
        uint8_t b = p[pos++];
        uint32_t run = (b >> 4) + 1;
        uint8_t c = b & 15;
        if ((b >> 4) == 15) {
            if (pos >= len) {
                break;
            }
            run = 16 + p[pos++];
        }
        if (run > total - done) {
            run = total - done;
        }
        if (c) {
            for (uint32_t j = 0; j < run; j++) {
                uint32_t a = ((uint32_t)((y >> 4) * bw + (x >> 4)) << 8) |
                             ((uint32_t)(y & 15) << 4) | (uint32_t)(x & 15);
                if (a & 1) {
                    s_ras_buf[a >> 1] |= (uint8_t)(c << 4);
                } else {
                    s_ras_buf[a >> 1] |= c;
                }
                if (++x == w) {
                    x = 0;
                    y++;
                }
            }
        } else {
            x += (int)run;
            while (x >= w) {
                x -= w;
                y++;
            }
        }
        done += run;
    }
    if (done != total) {
        return false;
    }

    s_ras.valid = true;
    s_ras.gen = gen;
    s_ras.w = w;
    s_ras.h = h;
    s_ras.bw = bw;
    s_ras.x0 = x0;
    s_ras.y0 = y0;
    s_ras.mpp = mm / 1000.0f;
    s_ras.inv_mpp = 1000.0f / (float)mm;
    s_ras.pal = pal;
    s_ras.bld = bld;
    if (pal) {
        for (int i = 0; i < 16; i++) {
            s_ras.pal16[i] = rd16(p + 16 + 2 * i);
        }
    }
    ESP_LOGI(TAG, "raster %dx%d mpp=%.2f pal=%d", w, h, s_ras.mpp, (int)pal);
    return true;
}

/* Class index of one raster cell (0 outside). */
static inline uint8_t raster_idx(int px, int py)
{
    if ((unsigned)px >= (unsigned)s_ras.w || (unsigned)py >= (unsigned)s_ras.h) {
        return 0;
    }
    uint32_t a = ((uint32_t)((py >> 4) * s_ras.bw + (px >> 4)) << 8) |
                 ((uint32_t)(py & 15) << 4) | (uint32_t)(px & 15);
    uint8_t byte = s_ras_buf[a >> 1];
    return (a & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 15);
}

static inline uint16_t raster_color(uint8_t c, const map_theme_t *th)
{
    if (s_ras.pal) {
        return s_ras.pal16[c];
    }
    /* Building layer (flag 0x4000): 5/7 = edge, 4/6 = roof (|2 keeps the
     * green class). We do not draw building shapes at all — the block is
     * simply filled with the underlying ground/park colour so the area
     * between roads reads as one solid block (Vice City style). */
    if (s_ras.bld && c >= 4 && c <= 7) {
        return (c & 2) ? th->park : th->block;
    }
    switch (c) {
    case 1:  return th->road;        /* theme road colour, like the firmware */
    case 2:  return th->park;
    case 3:  return th->water;
    default: return th->block;
    }
}

static inline uint16_t raster_px(int px, int py, const map_theme_t *th)
{
    return raster_color(raster_idx(px, py), th);
}

/* c0 -> c1, w = 0..256. RGB565 channels are interpolated separately. */
static inline uint16_t mix565(uint16_t a, uint16_t b, int w)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (((br - ar) * w) >> 8);
    int g = ag + (((bg - ag) * w) >> 8);
    int bl = ab + (((bb - ab) * w) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Inverse-project every screen pixel into the raster: handles rotation,
 * zoom and the round-screen mask in one pass. */
static void draw_raster(const map_theme_t *th)
{
    int cx = W / 2, cy = H / 2;
    int r2 = (W / 2 - 4) * (W / 2 - 4);
    for (int y = 0; y < H; y++) {
        float v = (float)(s_py - y);
        uint16_t *row = DST + y * W;
        for (int x = 0; x < W; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r2) {
                row[x] = 0x0000;
                continue;
            }
            float u = (float)(x - cx);
            float wx = s_posx + (u * s_cs + v * s_sn) * s_inv_sc;
            float wy = s_posy + (-u * s_sn + v * s_cs) * s_inv_sc;
            /* Bilinear sample (sample points at cell centres). The phone sends
             * a 2 m/px classification and the screen shows it magnified
             * ~1.5-2.4x, so nearest-neighbour sampling made road edges jump
             * one whole raster cell at a time. Blending the four neighbouring
             * cell colours turns that into a smooth ~2px ramp. */
            float fx = (wx - (float)s_ras.x0) * s_ras.inv_mpp - 0.5f;
            float fy = ((float)s_ras.y0 - wy) * s_ras.inv_mpp - 0.5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            int tx = (int)((fx - (float)ix) * 256.0f);
            int ty = (int)((fy - (float)iy) * 256.0f);
            uint16_t c00 = raster_px(ix, iy, th);
            uint16_t c10 = raster_px(ix + 1, iy, th);
            uint16_t c01 = raster_px(ix, iy + 1, th);
            uint16_t c11 = raster_px(ix + 1, iy + 1, th);
            if (c00 == c10 && c00 == c01 && c00 == c11) {
                row[x] = c00;   /* flat block: skip the blend */
            } else {
                row[x] = mix565(mix565(c00, c10, tx),
                                mix565(c01, c11, tx), ty);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* palettes (RGB565, same values as the MotoNav firmware)             */
/* ------------------------------------------------------------------ */
static const map_theme_t THEMES[12] = {
    {0xDEFB, 0x3186, 0x5DEB, 0x4C9B, 0xA21E, 0x58F1, 0xFFFF, 0xFE80, 0x94B3, 0x5AEC},
    {0xFFB9, 0x3187, 0x760D, 0x4E3B, 0xF258, 0x78EC, 0x6F5F, 0xF258, 0xA4B4, 0x62CC}, /* 1 罪恶都市：街区 #FFF5CD */
    {0x3A08, 0x9D15, 0x4327, 0x2A8F, 0xA2DF, 0x38EC, 0xFFFF, 0xFE87, 0x738F, 0x18E4},
    {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},
    {0xEF18, 0xF5CD, 0xC6B3, 0x9E3A, 0xB0A3, 0x5841, 0xFFFF, 0x298F, 0xA490, 0x6B0A},
    {0x0000, 0x39C7, 0x08A1, 0x0863, 0xFFFF, 0x39E7, 0xFFFF, 0xFFFF, 0x528A, 0x2104},
    {0x094B, 0xDF5F, 0x1A50, 0x2B35, 0xFE87, 0x08E8, 0xFFFF, 0xFE87, 0x6C76, 0x116B},
    {0x8E0E, 0xE7DA, 0x334A, 0x08C4, 0x08C4, 0xE7DA, 0xE7DA, 0x08C4, 0x334A, 0x8E0E},
    {0x00C1, 0x0B46, 0x0183, 0x0084, 0x57F0, 0x01C2, 0x97F5, 0xCFF9, 0x1AC6, 0x0101},
    {0x0863, 0x29AA, 0x08C4, 0x08A6, 0xFD80, 0x3940, 0xFF35, 0xFD80, 0x6AC7, 0x18A1},
    {0x5B08, 0x8C50, 0x3285, 0x29E8, 0x073F, 0x0188, 0xFFFF, 0xF9E7, 0xA514, 0x3186},
    {0x36C7, 0x0000, 0x0000, 0x0000, 0xFFF0, 0x0941, 0xFFFF, 0xFDC4, 0x7D69, 0x1283}, /* 11 龙珠雷达：绿色雷达底 */
};

static const map_theme_t THEMES_NIGHT[12] = {
    {0x18E5, 0x5B0E, 0x19E5, 0x118B, 0xA21E, 0x2848, 0xFFFF, 0xFE80, 0x4A8B, 0x0862},
    {0x20C7, 0x6271, 0x1207, 0x0A2B, 0xF258, 0x5047, 0x6F5F, 0xF258, 0x7370, 0x1063},
    {0x10C3, 0x5AED, 0x1163, 0x1107, 0xAB3F, 0x30AB, 0xEF5D, 0xFE87, 0x4229, 0x0841},
    {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},
    {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},
    {0x0000, 0x39C7, 0x08A1, 0x0863, 0xFFFF, 0x39E7, 0xFFFF, 0xFFFF, 0x528A, 0x2104},
    {0x00C7, 0xAE3D, 0x118B, 0x1A50, 0xFE87, 0x00A5, 0xFFFF, 0xFE87, 0x4B52, 0x08E7},
    {0x334A, 0x8E0E, 0x08C4, 0x08C4, 0xE7DA, 0x08C4, 0xE7DA, 0xE7DA, 0x334A, 0x08C4},
    {0x00C1, 0x0B46, 0x0183, 0x0084, 0x57F0, 0x01C2, 0x97F5, 0xCFF9, 0x1AC6, 0x0101},
    {0x0863, 0x29AA, 0x08C4, 0x08A6, 0xFD80, 0x3940, 0xFF35, 0xFD80, 0x6AC7, 0x18A1},
    {0x5B08, 0x8C50, 0x3285, 0x29E8, 0x073F, 0x0188, 0xFFFF, 0xF9E7, 0xA514, 0x3186},
    {0x1384, 0x0000, 0x0000, 0x0000, 0xFFF0, 0x0941, 0xFFFF, 0xFDC4, 0x7D69, 0x1283}, /* 11 龙珠雷达·夜 */
};

/* ------------------------------------------------------------------ */
/* raster helpers                                                     */
/* ------------------------------------------------------------------ */
static int16_t  *s_tx;   /* PSRAM scratch */
static int16_t  *s_ty;

static inline void put(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
        DST[y * W + x] = c;
    }
}

/* Blend c into the pixel with weight w (0..256), for edge anti-aliasing. */
static inline void put_mix(int x, int y, uint16_t c, int w)
{
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
        uint16_t *p = &DST[y * W + x];
        *p = mix565(*p, c, w);
    }
}

/* Scanline fill of an arbitrary polygon (even-odd). Polygons from the phone
 * are already simplified; very long ones are drawn as outlines to bound cost. */
static void fill_poly(const int16_t *xs, const int16_t *ys, int n, uint16_t c)
{
    if (n < 3) {
        return;
    }
    int miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    if (miny < 0) miny = 0;
    if (maxy >= H) maxy = H - 1;
    if (miny > maxy) {
        return;
    }
    for (int y = miny; y <= maxy; y++) {
        float inter[32];
        int cnt = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int y0 = ys[j], y1 = ys[i];
            if ((y0 > y) != (y1 > y)) {
                float x = (float)xs[j] + (float)(y - y0) * (xs[i] - xs[j]) / (float)(y1 - y0);
                if (cnt < 32) {
                    inter[cnt++] = x;
                }
            }
        }
        for (int a = 1; a < cnt; a++) {
            float v = inter[a];
            int b = a - 1;
            while (b >= 0 && inter[b] > v) {
                inter[b + 1] = inter[b];
                b--;
            }
            inter[b + 1] = v;
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            float fx0 = inter[a], fx1 = inter[a + 1];
            if (fx1 <= 0.0f || fx0 >= (float)W) {
                continue;
            }
            int xa = (int)floorf(fx0);
            int xb = (int)floorf(fx1);
            if (xa < 0) xa = 0;
            if (xb > W - 1) xb = W - 1;
            uint16_t *row = DST + y * W;
            for (int x = xa; x <= xb; x++) {
                /* Cover the edge pixels by their real fraction of the span
                 * instead of snapping to whole pixels: this is what removes
                 * the stair-steps on angled road/route edges. */
                float cov = fminf(fx1, (float)(x + 1)) - fmaxf(fx0, (float)x);
                if (cov <= 0.0f) {
                    continue;
                }
                if (cov >= 1.0f) {
                    row[x] = c;
                } else {
                    row[x] = mix565(row[x], c, (int)(cov * 256.0f));
                }
            }
        }
    }
}

static void fill_circle(int cx, int cy, float r, uint16_t c)
{
    int ir = (int)ceilf(r + 0.5f);
    float ro2 = (r + 0.5f) * (r + 0.5f);
    float ri = r - 0.5f;
    float ri2 = (ri > 0.0f) ? ri * ri : -1.0f;
    for (int y = -ir; y <= ir; y++) {
        int yy = y * y;
        for (int x = -ir; x <= ir; x++) {
            float d2 = (float)(x * x + yy);
            if (d2 > ro2) {
                continue;
            }
            if (d2 <= ri2) {
                put(cx + x, cy + y, c);   /* fully inside */
            } else {
                /* rim: blend by how much of the 1px band the pixel covers */
                float cov = r + 0.5f - sqrtf(d2);
                if (cov <= 0.0f) {
                    continue;
                }
                if (cov >= 1.0f) {
                    put(cx + x, cy + y, c);
                } else {
                    put_mix(cx + x, cy + y, c, (int)(cov * 256.0f));
                }
            }
        }
    }
}

static void thick_seg(int x0, int y0, int x1, int y1, float wpx, uint16_t c)
{
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) {
        return;
    }
    if (wpx < 1.0f) wpx = 1.0f;
    float nx = -dy / len * wpx * 0.5f;
    float ny = dx / len * wpx * 0.5f;
    int16_t qx[4] = {
        (int16_t)(x0 + nx), (int16_t)(x1 + nx),
        (int16_t)(x1 - nx), (int16_t)(x0 - nx)
    };
    int16_t qy[4] = {
        (int16_t)(y0 + ny), (int16_t)(y1 + ny),
        (int16_t)(y1 - ny), (int16_t)(y0 - ny)
    };
    fill_poly(qx, qy, 4, c);
    /* round caps/joins: hides the notches where segments meet at an angle */
    fill_circle(x0, y0, wpx * 0.5f, c);
    fill_circle(x1, y1, wpx * 0.5f, c);
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t  rds16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline int32_t  rd32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ------------------------------------------------------------------ */
/* projection                                                         */
/* ------------------------------------------------------------------ */
static inline void proj(float gx, float gy, int *sx, int *sy)
{
    float dx = gx - s_posx;
    float dy = gy - s_posy;
    *sx = s_cx + (int)lrintf(s_sc * (dx * s_cs - dy * s_sn));
    *sy = s_py - (int)lrintf(s_sc * (dx * s_sn + dy * s_cs));
}

static bool seg_visible(int x0, int y0, int x1, int y1)
{
    if (x0 < -40 && x1 < -40) return false;
    if (x0 > W + 40 && x1 > W + 40) return false;
    if (y0 < -40 && y1 < -40) return false;
    if (y0 > H + 40 && y1 > H + 40) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */
static void draw_map_chunk(const uint8_t *p, uint32_t len, const map_theme_t *th)
{
    if (len < 12) {
        return;
    }
    int32_t ox = rd32(p + 2), oy = rd32(p + 6);
    int n = rd16(p + 10);
    uint32_t pos = 12;
    float viewR = (float)(W / 2 + 80) / s_sc;

    for (int i = 0; i < n && pos + 3 <= len; i++) {
        uint8_t cls = p[pos];
        int np = rd16(p + pos + 1);
        pos += 3;
        if (np < 2 || pos + (uint32_t)np * 4 > len) {
            break;
        }
        /* bbox in map coordinates for culling */
        int16_t bx0 = 32767, by0 = 32767, bx1 = -32768, by1 = -32768;
        for (int k = 0; k < np; k++) {
            int16_t x = rds16(p + pos + 4 * k);
            int16_t y = rds16(p + pos + 4 * k + 2);
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
        float gx0 = ox + bx0 - s_posx, gx1 = ox + bx1 - s_posx;
        float gy0 = oy + by0 - s_posy, gy1 = oy + by1 - s_posy;
        if (gx1 < -viewR || gx0 > viewR || gy1 < -viewR || gy0 > viewR) {
            pos += (uint32_t)np * 4;
            continue;
        }

        bool poly = (cls & 0x80) != 0;
        int c = cls & 0x7F;
        int count = np > 4096 ? 4096 : np;
        for (int k = 0; k < count; k++) {
            float gx = (float)ox + rds16(p + pos + 4 * k);
            float gy = (float)oy + rds16(p + pos + 4 * k + 2);
            int sx, sy;
            proj(gx, gy, &sx, &sy);
            s_tx[k] = (int16_t)sx;
            s_ty[k] = (int16_t)sy;
        }
        if (poly) {
            uint16_t col = (c == 11) ? th->water : th->park;
            if (count <= 512) {
                fill_poly(s_tx, s_ty, count, col);
            } else {
                for (int k = 0; k + 1 < count; k++) {
                    thick_seg(s_tx[k], s_ty[k], s_tx[k + 1], s_ty[k + 1], 2.0f, col);
                }
            }
        } else {
            int rc = (c >= 1 && c <= 5) ? c : 5;
            static const float roadW[6] = { 0, 15, 12, 10, 7, 5 };
            float wpx = roadW[rc] * s_sc;
            if (wpx < 2.0f) wpx = 2.0f;
            for (int k = 0; k + 1 < count; k++) {
                if (seg_visible(s_tx[k], s_ty[k], s_tx[k + 1], s_ty[k + 1])) {
                    thick_seg(s_tx[k], s_ty[k], s_tx[k + 1], s_ty[k + 1], wpx, th->road);
                }
            }
        }
        pos += (uint32_t)np * 4;
    }
}

/* ------------------------------------------------------------------ */
/* route progress: how much of the route has been walked. Mirrors the  */
/* MotoNav firmware's updateRouteProgress(): a persistent segment+     */
/* offset position searched only in a local window once matched, with  */
/* projection onto segments (not vertices) and a 50 m off-route guard  */
/* so a corner's upcoming vertex / a looped-back segment can never     */
/* mark an unwalked stretch as done.                                   */
/* ------------------------------------------------------------------ */
static float    s_prog = -1.0f;       /* u = segment + offset, -1 = not matched */
static uint32_t s_prog_gen = 0;       /* route blob generation this progress belongs to */
static float    s_prog_cum = 0.0f;    /* cumulative metres along the route at s_prog */
static uint32_t s_prog_search_at = 0; /* ms of the last full-route search */

static void update_progress(const uint8_t *p, uint32_t len, uint32_t gen)
{
    if (len < 12) {
        return;
    }
    int n = rd16(p + 10);
    if (n < 2) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (gen != s_prog_gen) {
        /* A new route blob, even if it keeps the same frame (the phone
         * re-encodes/re-sends routes on style changes). The old segment /
         * cumulative distance no longer refers to this route, so re-locate
         * from scratch instead of reusing a stale progress that would mark
         * the whole route as walked. */
        s_prog_gen = gen;
        s_prog = -1.0f;
        s_prog_search_at = 0;
    }

    bool matched = s_prog >= 0.0f;
    if (!matched && now - s_prog_search_at < 1000) {
        return;   /* full search at most once per second while unmatched */
    }
    s_prog_search_at = now;

    float lo = -1e30f, hi = 1e30f;
    if (matched) {
        lo = s_prog_cum - 30.0f;    /* 30 m behind */
        hi = s_prog_cum + 300.0f;   /* 300 m ahead */
    }

    int32_t x = rd32(p + 2), y = rd32(p + 6);
    uint32_t q = 12;
    float cum = 0.0f;
    float best = 1e18f, bestU = -1.0f, bestCum = 0.0f;
    for (int k = 0; k + 1 < n; k++) {
        int32_t nx = x + rds16(p + q);
        int32_t ny = y + rds16(p + q + 2);
        q += 4;
        float vx = (float)(nx - x), vy = (float)(ny - y);
        float d0 = cum, d1 = cum + sqrtf(vx * vx + vy * vy);
        if (matched && (d1 < lo || d0 > hi)) {
            cum = d1;
            x = nx; y = ny;
            continue;
        }
        float ax = (float)x, ay = (float)y;
        float L2 = vx * vx + vy * vy;
        float t = (L2 > 0.0f) ? ((s_posx - ax) * vx + (s_posy - ay) * vy) / L2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float ex = ax + vx * t - s_posx, ey = ay + vy * t - s_posy;
        float d2 = ex * ex + ey * ey;
        if (d2 < best) {
            best = d2;
            bestU = (float)k + t;
            bestCum = d0 + (d1 - d0) * t;
        }
        cum = d1;
        x = nx; y = ny;
    }
    if (bestU < 0.0f || best > 50.0f * 50.0f) {
        return;   /* nothing in the window, or off-route: keep old progress */
    }
    s_prog = bestU;
    s_prog_cum = bestCum;
}

/* Constant-width polyline through the actual route points. Drawing through
 * the real vertices (round caps at every joint) keeps the line exactly on
 * the road; midpoint interpolation cut corners and drifted off the map. */
static void draw_route_poly(const int16_t *xs, const int16_t *ys, int n,
                            float wpx, uint16_t c)
{
    for (int i = 0; i + 1 < n; i++) {
        thick_seg(xs[i], ys[i], xs[i + 1], ys[i + 1], wpx, c);
    }
}

static void draw_route(const uint8_t *p, uint32_t len, const map_theme_t *th,
                       bool ghost, int trail_mode, uint32_t gen)
{
    if (len < 10) {
        return;
    }
    int32_t ax0 = rd32(p + 2), ay0 = rd32(p + 6);
    int n = rd16(p + 10);
    uint32_t pos = 12;
    if (n < 2 || pos + (uint32_t)(n - 1) * 4 > len) {
        return;
    }

    /* Walked boundary as segment + fraction, not a rounded segment index: the
     * colour change has to follow the rider continuously. With ceil() the
     * whole segment you just entered turned "done" at once.
     * trail_mode 1 = walked part grey, 2 = walked part erased (not drawn),
     * exactly like the firmware's "#r,1/2". */
    int done_seg = 0;
    float done_frac = 0.0f;
    if (!ghost) {
        update_progress(p, len, gen);
        if (s_prog >= 0.0f) {
            float u = s_prog;
            if (u > (float)(n - 1)) {
                u = (float)(n - 1);
            }
            done_seg = (int)u;
            done_frac = u - (float)done_seg;
            if (done_seg > n - 2) {
                done_seg = n - 2;
                done_frac = 1.0f;
            }
        }
    }

    /* Render a window around the player so long routes stay visible. */
    int start = done_seg - 2000;
    if (start < 0) {
        start = 0;
    }
    int count = n - start;
    if (count > 4096) {
        count = 4096;
    }
    if (count < 2) {
        return;
    }

    /* walk to `start`, then project the window */
    int32_t x = ax0, y = ay0;
    uint32_t q = pos;
    for (int k = 1; k <= start; k++) {
        x += rds16(p + q);
        y += rds16(p + q + 2);
        q += 4;
    }
    for (int k = 0; k < count; k++) {
        if (k > 0) {
            x += rds16(p + q);
            y += rds16(p + q + 2);
            q += 4;
        }
        int sx, sy;
        proj((float)x, (float)y, &sx, &sy);
        s_tx[k] = (int16_t)sx;
        s_ty[k] = (int16_t)sy;
    }

    float wpx = ghost ? 3.0f : 7.0f;

    /* Split vertex inside the window, plus the exact point part-way along the
     * segment it sits on. Projection is affine, so interpolating the projected
     * points matches interpolating on the road. */
    int idx = done_seg - start;
    if (idx < 0) {
        idx = 0;
    }
    if (idx > count - 1) {
        idx = count - 1;
    }
    bool split = (!ghost && idx + 1 < count && done_frac > 0.0f);
    int spx = 0, spy = 0;
    if (split) {
        spx = (int)lrintf((float)s_tx[idx] + (float)(s_tx[idx + 1] - s_tx[idx]) * done_frac);
        spy = (int)lrintf((float)s_ty[idx] + (float)(s_ty[idx + 1] - s_ty[idx]) * done_frac);
    }
    int tail = idx + (split ? 1 : 0);

    /* The un-walked route is the highlight: keep the theme hue but lift it a
     * little toward white and give it a dark halo of the same hue, so it
     * stays a clear highlight on every theme (several themes' route/done
     * colours are too close to the map or to each other). */
    uint16_t hi = blend565(th->route, 0xFFFF, 36);
    uint16_t lo = blend565(th->route, 0x0000, 130);
    /* dark outline first: masks the stair-stepped edge of the bright line */
    if (!ghost) {
        if (trail_mode == 1) {
            if (idx > 0) {
                draw_route_poly(s_tx, s_ty, idx + 1, wpx + 3.0f, th->done2);
            }
            if (split) {
                thick_seg(s_tx[idx], s_ty[idx], spx, spy, wpx + 3.0f, th->done2);
            }
        }
        if (split) {
            thick_seg(spx, spy, s_tx[idx + 1], s_ty[idx + 1], wpx + 3.0f, lo);
        }
        draw_route_poly(&s_tx[tail], &s_ty[tail], count - tail, wpx + 3.0f, lo);
    }
    if (!ghost && trail_mode == 1) {
        if (idx > 0) {
            draw_route_poly(s_tx, s_ty, idx + 1, wpx, th->done);
        }
        if (split) {
            thick_seg(s_tx[idx], s_ty[idx], spx, spy, wpx, th->done);
        }
    }
    if (split) {
        thick_seg(spx, spy, s_tx[idx + 1], s_ty[idx + 1], wpx, ghost ? th->done2 : hi);
    }
    draw_route_poly(&s_tx[tail], &s_ty[tail], count - tail, wpx, ghost ? th->done2 : hi);

    if (!ghost && start + count == n) {
        /* destination blip at the real route end (ring like the firmware) */
        int dx = s_tx[count - 1] - s_cx, dy = s_ty[count - 1] - s_py;
        if (dx * dx + dy * dy < (W / 2) * (W / 2)) {
            fill_circle(s_tx[count - 1], s_ty[count - 1], 11.0f, 0x0000);
            fill_circle(s_tx[count - 1], s_ty[count - 1], 9.0f, th->blip);
            fill_circle(s_tx[count - 1], s_ty[count - 1], 3.0f, 0x0000);
        }
    }
}

static void draw_player(const map_theme_t *th)
{
    /* black outline, then the hud-coloured body (points up = heading-up) */
    static const int16_t OUT[3][2] = { {0, -17}, {-13, 11}, {13, 11} };
    static const int16_t IN[3][2]  = { {0, -13}, { -9, 8},  { 9, 8}  };
    int16_t px[3], py[3];
    for (int k = 0; k < 3; k++) {
        px[k] = s_cx + OUT[k][0];
        py[k] = s_py + OUT[k][1];
    }
    fill_poly(px, py, 3, 0x0000);
    for (int k = 0; k < 3; k++) {
        px[k] = s_cx + IN[k][0];
        py[k] = s_py + IN[k][1];
    }
    fill_poly(px, py, 3, th->hud);
}

bool nav_map_render(uint16_t *dst, int w, int h, const nav_state_t *st)
{
    if (!dst || !st || !st->map_on || !st->active) {
        return false;
    }
    if (!s_tx) {
        s_tx = heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_ty = heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_tx || !s_ty) {
            ESP_LOGE(TAG, "no PSRAM for scratch");
            return false;
        }
    }

    const uint8_t *map = NULL, *ras = NULL;
    uint32_t map_len = 0, route_len = 0, ghost_len = 0, ras_len = 0, ras_gen = 0;
    uint32_t route_gen = 0;
    const uint8_t *route = NULL, *ghost = NULL;
    taskENTER_CRITICAL(&s_lock);
    map = s_buf[1]; map_len = s_len[1];
    route = s_buf[2]; route_len = s_len[2]; route_gen = s_gen[2];
    ras = s_buf[3]; ras_len = s_len[3]; ras_gen = s_gen[3];
    ghost = s_buf[4]; ghost_len = s_len[4];
    taskEXIT_CRITICAL(&s_lock);
    bool have_vec = map && map_len >= 12 && (int)rd16(map) == st->pos_frame;
    bool have_ras = ras && ras_len >= 16 && (int)rd16(ras) == st->pos_frame;
    if (!have_vec && !have_ras) {
        return false;   /* no basemap for the current position frame */
    }

    W = w;
    H = h;
    DST = dst;
    s_cx = w / 2;
    s_py = h / 2;                 /* player marker centred on screen */
    s_posx = st->pos_x_dm / 10.0f;
    s_posy = st->pos_y_dm / 10.0f;

    float speed = (float)st->speed_kmh;
    s_sc = 1.20f - speed * 0.008f;
    if (s_sc < 0.70f) s_sc = 0.70f;
    if (s_sc > 1.20f) s_sc = 1.20f;
    s_inv_sc = 1.0f / s_sc;
    float hd = (float)st->heading * (float)M_PI / 180.0f;
    s_cs = cosf(hd);
    s_sn = sinf(hd);

    int idx = st->theme;
    if (idx < 0 || idx >= 12) idx = 1;
    const map_theme_t *th = st->night ? &THEMES_NIGHT[idx] : &THEMES[idx];

    /* clear inside the circle, black outside */
    int r2 = (w / 2 - 4) * (w / 2 - 4);
    for (int y = 0; y < h; y++) {
        int dy = y - h / 2;
        uint16_t *row = dst + y * w;
        for (int x = 0; x < w; x++) {
            int dx = x - w / 2;
            row[x] = (dx * dx + dy * dy > r2) ? 0x0000 : th->block;
        }
    }

    bool drew = false;
    if (have_ras) {
        if (s_ras.gen != ras_gen) {
            if (!raster_decode(ras, ras_len, ras_gen)) {
                s_ras.valid = false;
                s_ras.gen = ras_gen;   /* do not retry the same broken blob */
            }
        }
        if (s_ras.valid) {
            draw_raster(th);
            drew = true;
        }
    }
    if (!drew && have_vec) {
        draw_map_chunk(map, map_len, th);
    }
    if (ghost && ghost_len >= 12) {
        draw_route(ghost, ghost_len, th, true, 1, 0);
    }
    /* the active route must belong to the current position frame, otherwise
     * a stale route is projected against a new position (wrong "done") */
    if (route && route_len >= 12 && (int)rd16(route) == st->pos_frame) {
        draw_route(route, route_len, th, false, st->trail == 2 ? 2 : 1, route_gen);
    }
    draw_player(th);
    return true;
}
