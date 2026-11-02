/*
 * Built-in watch faces for the Clock app. Each face is self-contained behind
 * the clock_face_t contract (see clock_faces.h). New faces are added as new
 * entries here without touching the app.
 */
#include "clock_faces.h"
#include "ui_theme.h"
#include "battery.h"
#include <time.h>
#include <stdio.h>
#include <math.h>

static const char *WK[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MO[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *WK_UP[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
                               "THURSDAY", "FRIDAY", "SATURDAY" };
static const char *MO_UP[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
                               "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

/* Until NTP/RTC provide a real time, run a self-contained demo clock so a face
 * never flashes a 1970 date right after boot. */
static uint32_t s_demo_secs = 10 * 3600 + 10 * 60;   /* starts at 10:10:00 */

/* ------------------------------------------------------------------ */
/* Big text face                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *time, *date;
} bigface_t;

static void *bigface_build(lv_obj_t *parent)
{
    bigface_t *f = lv_malloc(sizeof(*f));

    f->time = lv_label_create(parent);
    lv_label_set_text(f->time, "--:--");
    lv_obj_set_style_text_color(f->time, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(f->time, &clock_big, 0);
    lv_obj_align(f->time, LV_ALIGN_CENTER, 0, -26);

    f->date = lv_label_create(parent);
    lv_label_set_text(f->date, "---");
    lv_obj_set_style_text_color(f->date, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(f->date, &lv_font_montserrat_20, 0);
    lv_obj_align(f->date, LV_ALIGN_CENTER, 0, 34);

    return f;
}

static void bigface_tick(void *ctx)
{
    bigface_t *f = ctx;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[16];

    int hh, mm;
    if (tm.tm_year + 1900 < 2020) {   /* no NTP/RTC time yet: demo clock */
        s_demo_secs++;
        hh = (s_demo_secs / 3600) % 24;
        mm = (s_demo_secs / 60) % 60;
    } else {
        hh = tm.tm_hour; mm = tm.tm_min;
    }

    snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
    lv_label_set_text(f->time, b);
    if (tm.tm_year + 1900 >= 2020) {
        snprintf(b, sizeof(b), "%s, %s %d", WK[tm.tm_wday & 7], MO[tm.tm_mon & 11], tm.tm_mday);
    } else {
        snprintf(b, sizeof(b), "---");
    }
    lv_label_set_text(f->date, b);
}

static void bigface_destroy(void *ctx)
{
    lv_free(ctx);
}

/* ------------------------------------------------------------------ */
/* StarField face (rebuilt to match the user's Axure demo page_1)       */
/* ------------------------------------------------------------------ */
#define SF_MAIN 0xDEF1F5   /* theme */
#define SF_SEC  0x797979   /* secondary (progress track) */
#define SF_BLUE 0x06cfff   /* hour hand */
#define SF_RED  0xff0000   /* minute hand */

#define SF_ARC_R 132.0f    /* day band centreline radius (band 120..145, hugs ring 149) */
#define SF_ARC_W 25        /* day band thickness (120..145) */
#define SF_ARC_N 181       /* dense polyline (1 deg) -> smooth, no cracks */

typedef struct {
    lv_obj_t *hour_hand, *min_hand;
    lv_point_precise_t h_pts[2], m_pts[2];
    lv_point_precise_t tick_pts[8][2];
    lv_point_precise_t arc_pts[SF_ARC_N];
    lv_obj_t *day_track, *day_fill;
    lv_obj_t *batt_fill;
    lv_obj_t *time_l, *date_l, *wk_l;
    lv_obj_t *am_l, *pm_l;
    int last_min;
} starfield_t;

/* Current time with a self-contained demo fallback before NTP/RTC. */
static int clock_now(struct tm *tm)
{
    time_t now = time(NULL);
    localtime_r(&now, tm);
    if (tm->tm_year + 1900 < 2020) {
        s_demo_secs++;
        tm->tm_hour = (s_demo_secs / 3600) % 24;
        tm->tm_min  = (s_demo_secs / 60) % 60;
        tm->tm_sec  = s_demo_secs % 60;
        tm->tm_wday = 1;
        tm->tm_mon  = 0;
        tm->tm_mday = 1;
        return 0;
    }
    return 1;
}

static const char *ordinal_up(int d)
{
    if ((d % 100) / 10 == 1) return "TH";
    switch (d % 10) {
        case 1: return "ST";
        case 2: return "ND";
        case 3: return "RD";
        default: return "TH";
    }
}

static void *starfield_build(lv_obj_t *parent)
{
    starfield_t *f = lv_malloc(sizeof(*f));
    f->last_min = -1;

    /* black circular background */
    lv_obj_t *bg = lv_obj_create(parent);
    lv_obj_set_size(bg, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* outer ring: radius 148 */
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, 298, 298);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, 0, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 8 radial ticks at 45 deg, radius 125..139 (inside the ring, outside the
     * day band), perfectly centred on (180,180) */
    for (int i = 0; i < 8; i++) {
        float a = i * 45.0f * (float)M_PI / 180.0f;
        lv_point_precise_t *p = f->tick_pts[i];
        p[0] = (lv_point_precise_t){ (lv_coord_t)(180 + 96 * sinf(a)), (lv_coord_t)(180 - 96 * cosf(a)) };
        p[1] = (lv_point_precise_t){ (lv_coord_t)(180 + 112 * sinf(a)), (lv_coord_t)(180 - 112 * cosf(a)) };
        lv_obj_t *tk = lv_line_create(parent);
        lv_line_set_points(tk, p, 2);
        lv_obj_set_style_line_width(tk, 2, 0);
        lv_obj_set_style_line_color(tk, lv_color_hex(SF_MAIN), 0);
        lv_obj_set_style_line_rounded(tk, true, 0);
        lv_obj_clear_flag(tk, LV_OBJ_FLAG_CLICKABLE);
    }

    /* dial numbers 12/6/3/9 near the screen edge (outside the outer ring) */
    lv_obj_t *n12 = lv_label_create(parent);
    lv_label_set_text(n12, "12");
    lv_obj_set_style_text_color(n12, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(n12, &hooge25, 0);
    lv_obj_align(n12, LV_ALIGN_CENTER, 0, -165);

    lv_obj_t *n6 = lv_label_create(parent);
    lv_label_set_text(n6, "6");
    lv_obj_set_style_text_color(n6, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(n6, &hooge25, 0);
    lv_obj_align(n6, LV_ALIGN_CENTER, 5, 164);

    lv_obj_t *n3 = lv_label_create(parent);
    lv_label_set_text(n3, "3");
    lv_obj_set_style_text_color(n3, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(n3, &hooge25, 0);
    lv_obj_align(n3, LV_ALIGN_CENTER, 164, 0);

    lv_obj_t *n9 = lv_label_create(parent);
    lv_label_set_text(n9, "9");
    lv_obj_set_style_text_color(n9, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(n9, &hooge25, 0);
    lv_obj_align(n9, LV_ALIGN_CENTER, -164, 0);

    /* day-progress: bottom semicircle (opening UP), centred on (180,180),
     * radius 111, thickness 22 (band 100..122). Runs from the LEFT end over the
     * bottom to the RIGHT end; track grey, fill theme, grows left -> right. */
    for (int i = 0; i < SF_ARC_N; i++) {
        float a = (160.0f - 140.0f * i / (SF_ARC_N - 1)) * (float)M_PI / 180.0f;  /* 160..20 deg: ends pulled in, clear of AM/PM */
        f->arc_pts[i] = (lv_point_precise_t){
            (lv_coord_t)(180 + SF_ARC_R * cosf(a)),
            (lv_coord_t)(180 + SF_ARC_R * sinf(a))
        };
    }
    f->day_track = lv_line_create(parent);
    lv_line_set_points(f->day_track, f->arc_pts, SF_ARC_N);
    lv_obj_set_style_line_width(f->day_track, SF_ARC_W, 0);
    lv_obj_set_style_line_color(f->day_track, lv_color_hex(SF_SEC), 0);
    lv_obj_set_style_line_rounded(f->day_track, true, 0);
    lv_obj_clear_flag(f->day_track, LV_OBJ_FLAG_CLICKABLE);

    f->day_fill = lv_line_create(parent);
    lv_line_set_points(f->day_fill, f->arc_pts, 2);
    lv_obj_set_style_line_width(f->day_fill, SF_ARC_W, 0);
    lv_obj_set_style_line_color(f->day_fill, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_line_rounded(f->day_fill, true, 0);
    lv_obj_clear_flag(f->day_fill, LV_OBJ_FLAG_CLICKABLE);

    /* centre text: MONDAY / big time / JANUARY 1ST */
    f->wk_l = lv_label_create(parent);
    lv_label_set_text(f->wk_l, "MONDAY");
    lv_obj_set_style_text_color(f->wk_l, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(f->wk_l, &hooge20, 0);
    lv_obj_align(f->wk_l, LV_ALIGN_CENTER, 0, -30);

    f->time_l = lv_label_create(parent);
    lv_label_set_text(f->time_l, "09:29");
    lv_obj_set_style_text_color(f->time_l, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(f->time_l, &hooge55, 0);
    lv_obj_align(f->time_l, LV_ALIGN_CENTER, 0, 0);

    f->date_l = lv_label_create(parent);
    lv_label_set_text(f->date_l, "JANUARY 1ST");
    lv_obj_set_style_text_color(f->date_l, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_text_font(f->date_l, &hooge17, 0);
    lv_obj_align(f->date_l, LV_ALIGN_CENTER, 0, 36);

    /* AM/PM: right of the time, sharp rectangles. The active one is filled with
     * the theme colour and uses black text; the inactive one shows theme text. */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *lab = lv_label_create(parent);
        lv_label_set_text(lab, i ? "PM" : "AM");
        lv_obj_set_style_text_color(lab, lv_color_hex(SF_MAIN), 0);
        lv_obj_set_style_text_font(lab, &hooge17, 0);
        lv_obj_set_style_bg_color(lab, lv_color_hex(SF_MAIN), 0);
        lv_obj_set_style_bg_opa(lab, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(lab, 0, 0);
        lv_obj_set_style_pad_hor(lab, 7, 0);
        lv_obj_set_style_pad_ver(lab, 0, 0);
        lv_obj_align(lab, LV_ALIGN_CENTER, 108, i ? 11 : -11);
        if (i == 0) {
            f->am_l = lab;
        } else {
            f->pm_l = lab;
        }
    }

    /* battery: small horizontal bar (grey track + theme fill) + a red dot below */
    lv_obj_t *btr = lv_obj_create(parent);
    lv_obj_remove_style_all(btr);
    lv_obj_set_pos(btr, 146, 239);
    lv_obj_set_size(btr, 66, 5);
    lv_obj_set_style_bg_color(btr, lv_color_hex(SF_SEC), 0);
    lv_obj_set_style_bg_opa(btr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(btr, LV_OBJ_FLAG_CLICKABLE);
    f->batt_fill = lv_obj_create(btr);
    lv_obj_remove_style_all(f->batt_fill);
    lv_obj_set_pos(f->batt_fill, 0, 0);
    lv_obj_set_size(f->batt_fill, 0, 5);
    lv_obj_set_style_bg_color(f->batt_fill, lv_color_hex(SF_MAIN), 0);
    lv_obj_set_style_bg_opa(f->batt_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(f->batt_fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *red = lv_obj_create(parent);
    lv_obj_remove_style_all(red);
    lv_obj_set_pos(red, 176, 249);
    lv_obj_set_size(red, 6, 5);
    lv_obj_set_style_bg_color(red, lv_color_hex(0xB80000), 0);
    lv_obj_set_style_bg_opa(red, LV_OPA_COVER, 0);
    lv_obj_clear_flag(red, LV_OBJ_FLAG_CLICKABLE);

    /* hands: SHORT edge marks confined to the annulus between the outer ring
     * (148) and the numbers (~165), drawn as thick rounded lines */
    f->hour_hand = lv_line_create(parent);
    lv_obj_set_style_line_width(f->hour_hand, 10, 0);
    lv_obj_set_style_line_color(f->hour_hand, lv_color_hex(SF_BLUE), 0);
    lv_obj_set_style_line_rounded(f->hour_hand, true, 0);
    f->h_pts[0] = (lv_point_precise_t){ 0, 0 };
    f->h_pts[1] = (lv_point_precise_t){ 0, 0 };
    lv_line_set_points(f->hour_hand, f->h_pts, 2);
    lv_obj_clear_flag(f->hour_hand, LV_OBJ_FLAG_CLICKABLE);

    f->min_hand = lv_line_create(parent);
    lv_obj_set_style_line_width(f->min_hand, 5, 0);
    lv_obj_set_style_line_color(f->min_hand, lv_color_hex(SF_RED), 0);
    lv_obj_set_style_line_rounded(f->min_hand, true, 0);
    f->m_pts[0] = (lv_point_precise_t){ 0, 0 };
    f->m_pts[1] = (lv_point_precise_t){ 0, 0 };
    lv_line_set_points(f->min_hand, f->m_pts, 2);
    lv_obj_clear_flag(f->min_hand, LV_OBJ_FLAG_CLICKABLE);


    return f;
}

static void starfield_tick(void *ctx)
{
    starfield_t *f = ctx;
    struct tm tm;
    int real = clock_now(&tm);
    if (tm.tm_min == f->last_min) {
        return;   /* update once per minute (no seconds) */
    }
    f->last_min = tm.tm_min;

    int hh = tm.tm_hour, mm = tm.tm_min;
    int h12 = hh % 12;
    if (h12 == 0) h12 = 12;   /* 12h with the display hour 12 for midnight/noon */
    int disp = hh % 12;
    if (disp == 0) disp = 12;
    char b[32];

    snprintf(b, sizeof(b), "%02d:%02d", disp, mm);
    lv_label_set_text(f->time_l, b);
    if (real) {
        snprintf(b, sizeof(b), "%s", WK_UP[tm.tm_wday & 7]);
        lv_label_set_text(f->wk_l, b);
        snprintf(b, sizeof(b), "%s %d%s", MO_UP[tm.tm_mon & 11], tm.tm_mday, ordinal_up(tm.tm_mday));
        lv_label_set_text(f->date_l, b);
    }

    /* AM/PM: only the active period is a highlighted (theme) box with black
     * text; the inactive one has no background and uses theme-coloured text */
    bool am = (hh < 12);
    lv_obj_set_style_bg_opa(f->am_l, am ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(f->pm_l, am ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(f->am_l, lv_color_hex(am ? 0x000000 : SF_MAIN), 0);
    lv_obj_set_style_text_color(f->pm_l, lv_color_hex(am ? SF_MAIN : 0x000000), 0);

    /* hands: angle 0 = 12 o'clock, clockwise, radius 152..165 */
    float ha = (h12 * 30.0f + mm * 0.5f) * (float)M_PI / 180.0f;
    float ma = (mm * 6.0f) * (float)M_PI / 180.0f;
    f->h_pts[0] = (lv_point_precise_t){ (lv_coord_t)(180 + 156 * sinf(ha)), (lv_coord_t)(180 - 156 * cosf(ha)) };
    f->h_pts[1] = (lv_point_precise_t){ (lv_coord_t)(180 + 167 * sinf(ha)), (lv_coord_t)(180 - 167 * cosf(ha)) };
    lv_line_set_points(f->hour_hand, f->h_pts, 2);
    f->m_pts[0] = (lv_point_precise_t){ (lv_coord_t)(180 + 156 * sinf(ma)), (lv_coord_t)(180 - 156 * cosf(ma)) };
    f->m_pts[1] = (lv_point_precise_t){ (lv_coord_t)(180 + 167 * sinf(ma)), (lv_coord_t)(180 - 167 * cosf(ma)) };
    lv_line_set_points(f->min_hand, f->m_pts, 2);

    /* day progress: fill from the left end toward the right as the day elapses */
    int day_sec = hh * 3600 + mm * 60 + (real ? tm.tm_sec : 0);
    int n = 2 + (SF_ARC_N - 2) * day_sec / 86400;
    if (n > SF_ARC_N) n = SF_ARC_N;
    lv_line_set_points(f->day_fill, f->arc_pts, n);

    /* battery */
    int pct = battery_get_percent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_set_width(f->batt_fill, (lv_coord_t)(66 * pct / 100));
}

static void starfield_destroy(void *ctx)
{
    lv_free(ctx);
}

/* ------------------------------------------------------------------ */
/* face registry                                                       */
/* ------------------------------------------------------------------ */
const clock_face_t clock_faces[] = {
    { .name = "Big Text", .build = bigface_build, .tick = bigface_tick, .destroy = bigface_destroy },
    { .name = "StarField", .build = starfield_build, .tick = starfield_tick, .destroy = starfield_destroy },
};
const int clock_face_count = (int)(sizeof(clock_faces) / sizeof(clock_faces[0]));
