// The two-eyed character: spring physics, squash & stretch, tilt, blink, gaze and expressions.
// Ported from the browser simulator (same constants), drawn with LVGL lines/triangles so it can rotate.
#include "character.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

LV_FONT_DECLARE(font_bold18);

#define LS 1.8f      // exaggeration
#define SP 0.65f     // speed
character_t CH;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static float s_next_blink, s_blink_t = -1, s_idle_look, s_cock, s_cock_until;
static float s_now;
static float s_rb[4] = { -60, 60, -30, 30 };
static float s_eb[4];                    // the two eyes alone (screen coords), for keeping effects clear of them

void ch_reset(int w, int h, float hx, float hy, float scale)
{
    float keep_breath_amp = 2, keep_breath_ms = 650;
    memset(&CH, 0, sizeof CH);
    CH.w = w; CH.h = h;
    CH.x = CH.tx = CH.home_x = hx; CH.y = CH.ty = CH.home_y = hy;
    CH.k = 0.012f * SP * SP; CH.c = 0.16f * SP; CH.maxv = 4.5f * SP;
    CH.scale = CH.base = scale; CH.scale_t = -1; CH.alpha = 1;
    CH.open = 1; CH.breath_amp = keep_breath_amp; CH.breath_ms = keep_breath_ms;
    CH.page_flip = -1; CH.stars_t = -1e9f; CH.spin_t0 = -1e9f;
    CH.fx0 = CH.fx1 = hx; CH.fy0 = CH.fy1 = hy;
    s_rb[0] = -60 * scale; s_rb[1] = 60 * scale; s_rb[2] = -30 * scale; s_rb[3] = 30 * scale;
    CH.bx0 = hx - 60 * scale; CH.bx1 = hx + 60 * scale; CH.by0 = hy - 30 * scale; CH.by1 = hy + 30 * scale;
    CH.lx0 = 2; CH.ly0 = 2; CH.lx1 = w - 2; CH.ly1 = h - 2;
}

void ch_free_box(float x0, float x1, float y0, float y1) { CH.fx0 = x0; CH.fx1 = x1; CH.fy0 = y0; CH.fy1 = y1; }

void ch_seek(float x, float y, float k, float maxv)
{
    if (fabsf(x - CH.tx) + fabsf(y - CH.ty) > 30) CH.bend = (frand() < .5f ? -1 : 1) * (0.3f + frand() * 0.3f);
    CH.grav = false; CH.tx = x; CH.ty = y;
    CH.k = (k > 0 ? k : 0.012f) * SP * SP; CH.c = 0.16f * SP; CH.maxv = (maxv > 0 ? maxv : 4.5f) * SP;
}

void ch_hop(float v) { CH.grav = true; CH.floor_y = CH.home_y; CH.tx = CH.home_x; CH.vy = -v * SP; }
void ch_burst_stars(void) { CH.stars_t = s_now; }
void ch_spin_in(float deg) { CH.spin_from = deg; CH.spin_t0 = s_now; CH.tilt = deg; CH.tv = 0; CH.expr = EX_SURPRISED; }

static float blink(float t)
{
    if (s_blink_t < 0 && t > s_next_blink) s_blink_t = t;
    if (s_blink_t >= 0) {
        float d = t - s_blink_t;
        if (d > 170) { s_blink_t = -1; s_next_blink = t + 2000 + frand() * 3500; return 1; }
        return fabsf(cosf(d / 170 * (float)M_PI));
    }
    return 1;
}

static void idle_behaviour(float t)
{
    if (t > s_idle_look) {
        float sx = fminf(30, (CH.fx1 - CH.fx0) / 2), sy = fminf(24, (CH.fy1 - CH.fy0) / 2);
        ch_seek(clampf(CH.home_x + (frand() * 2 - 1) * sx, CH.fx0, CH.fx1), clampf(CH.home_y + (frand() * 2 - 1) * sy, CH.fy0, CH.fy1), 0.008f, 2);
        s_idle_look = t + 1200 + frand() * 1800;
    }
    if (t > s_cock_until) { s_cock = frand() < .5f ? 0 : (frand() < .5f ? -1 : 1) * (9 + frand() * 6); s_cock_until = t + 1100 + frand() * 1600; }
}

// cached render values
static float r_lx, r_rx, r_ecy, r_cy, r_sqx, r_sqy, r_px, r_py;

void ch_step(float t)
{
    s_now = t;
    if (CH.idle) idle_behaviour(t);
    CH.pvx = CH.vx; CH.pvy = CH.vy;
    const float g2 = 0.22f * SP * SP;
    if (CH.grav) {
        CH.vy += g2; CH.vx = (CH.tx - CH.x) * 0.05f; CH.x += CH.vx; CH.y += CH.vy;
        if (CH.y > CH.floor_y && CH.vy > 0) {
            CH.y = CH.floor_y;
            if (CH.vy > 1.2f * SP) { CH.vy = -CH.vy * 0.3f; CH.impv += 0.15f * LS; } else { CH.vy = 0; CH.grav = false; CH.tx = CH.home_x; CH.ty = CH.home_y; }
        }
    } else {
        float dx = CH.tx - CH.x, dy = CH.ty - CH.y, dist = sqrtf(dx * dx + dy * dy);
        float bf = CH.bend * clampf((dist - 20) / 100, 0, 1);
        CH.vx += dx * CH.k - dy * CH.k * bf - CH.vx * CH.c;
        CH.vy += dy * CH.k + dx * CH.k * bf - CH.vy * CH.c;
        float sp = sqrtf(CH.vx * CH.vx + CH.vy * CH.vy), mx = CH.maxv * (1 + (LS - 1) * .2f);
        if (sp > mx) { CH.vx *= mx / sp; CH.vy *= mx / sp; }
        CH.x += CH.vx; CH.y += CH.vy;
    }
    CH.dist += sqrtf(CH.vx * CH.vx + CH.vy * CH.vy);
    // keep inside the screen
    if (CH.by0 < CH.ly0) { CH.y += (CH.ly0 - CH.by0) * .6f; if (CH.vy < 0) CH.vy = 0; }
    if (CH.by1 > CH.ly1) { CH.y -= (CH.by1 - CH.ly1) * .6f; if (CH.vy > 0 && !CH.grav) CH.vy = 0; }
    if (CH.bx0 < CH.lx0) { CH.x += (CH.lx0 - CH.bx0) * .6f; if (CH.vx < 0) CH.vx = 0; }
    if (CH.bx1 > CH.lx1) { CH.x -= (CH.bx1 - CH.lx1) * .6f; if (CH.vx > 0) CH.vx = 0; }

    if (CH.scale_t > 0) CH.scale += (CH.scale_t - CH.scale) * .12f;
    float S2 = CH.scale, spd = sqrtf(CH.vx * CH.vx + CH.vy * CH.vy);
    bool still = CH.grav;
    float nsp = spd / SP, mv = still ? 0 : fminf(1, nsp / 1.5f);
    float ph = fabsf(sinf(CH.dist / (26 * S2) * (float)M_PI));
    CH.impv += (0 - CH.imp) * .12f; CH.impv *= .78f; CH.imp += CH.impv;
    CH.acc += (((CH.vx - CH.pvx) / SP * 25) - CH.acc) * .08f;
    float vxn = still ? 0 : CH.vx / SP / S2, vyn = still ? 0 : CH.vy / SP / S2;
    if (CH.gaze_ov_on) { CH.gx += (CH.gaze_x - CH.gx) * .3f; CH.gy += (CH.gaze_y - CH.gy) * .3f; }
    else { CH.gx += (clampf(vxn * 3.5f * LS, -16, 16) - CH.gx) * .12f; CH.gy += (clampf(vyn * 3.2f * LS, -12, 12) - CH.gy) * .12f; }
    float stretchX = 1 + clampf(fabsf(vxn) * .06f * LS, 0, .55f) - clampf(fabsf(vyn) * .045f * LS, 0, .32f), stretchY = 1 / stretchX;
    float hopSq = 1 + (ph - .5f) * .08f * LS * mv;
    bool noBlink = CH.expr == EX_STRAIN || CH.expr == EX_HAPPY || CH.expr == EX_SLEEP;
    float o = noBlink ? 1 : blink(t);
    if (CH.expr == EX_PEEK) o = fminf(o, .9f);
    CH.open += (o - CH.open) * .45f;
    float moveTilt = clampf((vxn * 4.2f + (still ? 0 : CH.acc) * .6f) * LS, -34, 34);
    float tgt = CH.tilt_ov_on ? CH.tilt_ov : moveTilt + (CH.idle ? s_cock : 0);
    CH.tv += (tgt - CH.tilt) * .06f; CH.tv *= .78f; CH.tilt += CH.tv;
    // scripted spin after the device is turned
    if (t - CH.spin_t0 < 640) {
        float q = clampf((t - CH.spin_t0 - 120) / 520, 0, 1);
        float eb = 1 + 2.4f * powf(q - 1, 3) + 1.4f * powf(q - 1, 2);
        CH.tilt = CH.spin_from * (1 - eb); CH.tv = 0;
    }
    CH.book_a += ((CH.book ? 1.f : 0.f) - CH.book_a) * .1f;
    if (t - CH.spin_t0 < 640) CH.umb_a = 0;            // closed while spinning upright after a turn
    else CH.umb_a += ((CH.umbrella ? 1.f : 0.f) - CH.umb_a) * .08f;
    float hp = -ph * (4 + 2.5f * LS) * mv * S2;
    float bob = sinf(t / CH.breath_ms) * CH.breath_amp * (1 - mv) * S2;
    float cy = CH.y + hp + bob;
    float sqx = stretchX * (2 - hopSq) * (1 + CH.imp), sqy = stretchY * hopSq / (1 + CH.imp);
    float sp2 = 1 + (stretchX - 1) * .4f;
    r_lx = CH.x - 38 * sp2 + CH.gx; r_rx = CH.x + 38 * sp2 + CH.gx; r_ecy = cy + CH.gy; r_cy = cy;
    r_sqx = sqx; r_sqy = sqy; r_px = CH.x; r_py = cy + 18 * S2;
    // rotated bounding box (with hysteresis: grows at once, shrinks slowly)
    float a = CH.tilt * (float)M_PI / 180, ca = cosf(a), sa = sinf(a), hw = 19 * sqx, hh = 24 * fmaxf(sqy, 1);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    float pts[12][2]; int n = 0;
    float ex[2] = { r_lx, r_rx };
    for (int e = 0; e < 2; e++) { pts[n][0] = ex[e] - hw; pts[n++][1] = r_ecy - hh; pts[n][0] = ex[e] + hw; pts[n++][1] = r_ecy - hh;
                                  pts[n][0] = ex[e] + hw; pts[n++][1] = r_ecy + hh; pts[n][0] = ex[e] - hw; pts[n++][1] = r_ecy + hh; }
    if (CH.book_a > .5f) { float bx = CH.x + CH.gx * .35f, by = r_ecy + 32; pts[n][0] = bx - 30; pts[n++][1] = by - 8; pts[n][0] = bx + 30; pts[n++][1] = by + 12; }
    if (CH.umb_a > .5f) { pts[n][0] = CH.x - 60; pts[n++][1] = cy - 70; pts[n][0] = CH.x + 60; pts[n++][1] = cy - 70; }   // umbrella: screen edges keep it in view
    float e0 = 1e9f, e1 = -1e9f, f0 = 1e9f, f1 = -1e9f;
    for (int i = 0; i < n; i++) {
        float sx = CH.x + (pts[i][0] - CH.x) * S2, sy = cy + (pts[i][1] - cy) * S2, u = sx - r_px, v = sy - r_py;
        float qx = r_px + u * ca - v * sa, qy = r_py + u * sa + v * ca;
        x0 = fminf(x0, qx); x1 = fmaxf(x1, qx); y0 = fminf(y0, qy); y1 = fmaxf(y1, qy);
        if (i < 8) { e0 = fminf(e0, qx); e1 = fmaxf(e1, qx); f0 = fminf(f0, qy); f1 = fmaxf(f1, qy); }
    }
    s_eb[0] = e0; s_eb[1] = f0; s_eb[2] = e1; s_eb[3] = f1;
    // box kept relative to the body (like the simulator): grows at once, shrinks slowly
    float nb[4] = { x0 - CH.x, x1 - CH.x, y0 - CH.y, y1 - CH.y };
    s_rb[0] = fminf(nb[0], s_rb[0] * .9f + nb[0] * .1f); s_rb[1] = fmaxf(nb[1], s_rb[1] * .9f + nb[1] * .1f);
    s_rb[2] = fminf(nb[2], s_rb[2] * .9f + nb[2] * .1f); s_rb[3] = fmaxf(nb[3], s_rb[3] * .9f + nb[3] * .1f);
    CH.bx0 = CH.x + s_rb[0]; CH.bx1 = CH.x + s_rb[1]; CH.by0 = CH.y + s_rb[2]; CH.by1 = CH.y + s_rb[3];
    // hard limit, this very frame: nothing it draws may reach the other elements on the screen
    float dx = 0, dy = 0;
    if (CH.by1 > CH.ly1) dy = CH.ly1 - CH.by1; else if (CH.by0 < CH.ly0) dy = CH.ly0 - CH.by0;
    if (CH.bx1 > CH.lx1) dx = CH.lx1 - CH.bx1; else if (CH.bx0 < CH.lx0) dx = CH.lx0 - CH.bx0;
    if (dx || dy) {
        CH.x += dx; CH.y += dy; CH.bx0 += dx; CH.bx1 += dx; CH.by0 += dy; CH.by1 += dy;
        s_eb[0] += dx; s_eb[2] += dx; s_eb[1] += dy; s_eb[3] += dy;
        r_px += dx; r_py += dy; r_cy += dy; r_ecy += dy; r_lx += dx; r_rx += dx;
    }
}

// What the next draw would look like, quantized to half pixels. If it's unchanged the UI skips
// redrawing the character's area entirely (no render, no transfer: saves battery when idle).
uint32_t ch_signature(void)
{
    uint32_t h = 2166136261u;
    #define MIX(v) do { h ^= (uint32_t)(int32_t)(v); h *= 16777619u; } while (0)
    MIX(lroundf(r_lx * 2)); MIX(lroundf(r_rx * 2)); MIX(lroundf(r_ecy * 2)); MIX(lroundf(r_cy * 2));
    MIX(lroundf(r_sqx * 40)); MIX(lroundf(r_sqy * 40)); MIX(lroundf(CH.tilt * 4)); MIX(lroundf(CH.open * 40));
    MIX(CH.expr); MIX(lroundf(CH.scale * 200)); MIX(lroundf(CH.alpha * 50));
    MIX(lroundf(CH.book_a * 30)); MIX(lroundf(CH.page_flip * 40)); MIX(lroundf(CH.umb_a * 30)); MIX(lroundf(CH.snow_cap * 30));
    if (CH.think_on) MIX(lroundf(CH.think_e / 16) + lroundf(s_now / 33));
    if (CH.zzz) MIX(lroundf(s_now / 33));
    float q = (s_now - CH.stars_t) / 700; if (q >= 0 && q < 1.05f) MIX(lroundf(s_now / 16));
    #undef MIX
    return h;
}

// ------------------------------------------------------------------ umbrella geometry (head space)
#define UMB_RX 60.f
#define UMB_RY 30.f
#define UMB_BASE 34.f          // canopy rim sits this far above the eyes' centre
// The umbrella rises into place as it appears (umb_a 0..1).
static float umb_lift(void) { float a = CH.umb_a; return (1 - a) * 26; }

static void to_head(float x, float y, float *hx, float *hy)
{
    float a = -CH.tilt * (float)M_PI / 180, ca = cosf(a), sa = sinf(a);
    float u = x - r_px, v = y - r_py;
    float sx = r_px + u * ca - v * sa, sy = r_py + u * sa + v * ca;
    *hx = CH.x + (sx - CH.x) / CH.scale; *hy = r_cy + (sy - r_cy) / CH.scale;
}

void ch_umbrella_edge(float side, float *x, float *y)
{
    float hx = CH.x + side * UMB_RX, hy = r_cy - UMB_BASE + umb_lift();
    float sx = CH.x + (hx - CH.x) * CH.scale, sy = r_cy + (hy - r_cy) * CH.scale, u = sx - r_px, v = sy - r_py;
    float a = CH.tilt * (float)M_PI / 180;
    *x = r_px + u * cosf(a) - v * sinf(a); *y = r_py + u * sinf(a) + v * cosf(a);
}

void ch_eye_box(float *x0, float *y0, float *x1, float *y1) { *x0 = s_eb[0]; *y0 = s_eb[1]; *x1 = s_eb[2]; *y1 = s_eb[3]; }

bool ch_umbrella_hit(float x, float y)
{
    if (CH.umb_a < .5f) return false;
    float hx, hy; to_head(x, y, &hx, &hy);
    float by = r_cy - UMB_BASE + umb_lift(), dx = (hx - CH.x) / UMB_RX;
    if (fabsf(dx) >= 1) return false;
    return hy >= by - UMB_RY * sqrtf(1 - dx * dx) && hy <= by + 6;
}

// ------------------------------------------------------------------ drawing
static lv_layer_t *L;
static float d_ca, d_sa, d_S2;
static lv_opa_t d_opa;

static void xf(float x, float y, float *ox, float *oy)
{
    float sx = CH.x + (x - CH.x) * d_S2, sy = r_cy + (y - r_cy) * d_S2, u = sx - r_px, v = sy - r_py;
    *ox = r_px + u * d_ca - v * d_sa; *oy = r_py + u * d_sa + v * d_ca;
}

// ---- drawing goes through these, so the same code can also just measure what it would draw
// (ch_measure): the screen then redraws only that box instead of a generous guess around the body.
static bool s_meas; static int32_t s_mb[4]; static uint32_t s_mh;   // box and fingerprint of what is drawn
static void mh(float v) { s_mh = (s_mh ^ (uint32_t)lroundf(v * 256)) * 16777619u; }
static void mb_add(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 > x1 || y0 > y1) return;
    if (x0 - 2 < s_mb[0]) s_mb[0] = x0 - 2;
    if (y0 - 2 < s_mb[1]) s_mb[1] = y0 - 2;
    if (x1 + 2 > s_mb[2]) s_mb[2] = x1 + 2;
    if (y1 + 2 > s_mb[3]) s_mb[3] = y1 + 2;
}
static void d_rect(lv_layer_t *l, const lv_draw_rect_dsc_t *d, const lv_area_t *a) { mb_add(a->x1, a->y1, a->x2, a->y2); if (s_meas) { mh(a->x1); mh(a->y1); mh(a->x2); mh(a->y2); mh(d->radius); mh(d->bg_opa); mh(lv_color_to_u32(d->bg_color) & 0xffffff); return; } lv_draw_rect(l, d, a); }
static void d_label(lv_layer_t *l, const lv_draw_label_dsc_t *d, const lv_area_t *a) { mb_add(a->x1, a->y1, a->x2, a->y2); if (s_meas) { mh(a->x1); mh(a->y1); mh(d->opa); mh(d->text ? d->text[0] : 0); return; } lv_draw_label(l, d, a); }
static void d_line(lv_layer_t *l, const lv_draw_line_dsc_t *d)
{
    int32_t h = d->width / 2 + 1;
    mb_add((int32_t)fminf(d->p1.x, d->p2.x) - h, (int32_t)fminf(d->p1.y, d->p2.y) - h, (int32_t)fmaxf(d->p1.x, d->p2.x) + h, (int32_t)fmaxf(d->p1.y, d->p2.y) + h);
    if (s_meas) { mh(d->p1.x); mh(d->p1.y); mh(d->p2.x); mh(d->p2.y); mh(d->width); mh(d->opa); mh(lv_color_to_u32(d->color) & 0xffffff); return; }
    lv_draw_line(l, d);
}
static void d_tri(lv_layer_t *l, const lv_draw_triangle_dsc_t *d)
{
    float x0 = fminf(d->p[0].x, fminf(d->p[1].x, d->p[2].x)), x1 = fmaxf(d->p[0].x, fmaxf(d->p[1].x, d->p[2].x));
    float y0 = fminf(d->p[0].y, fminf(d->p[1].y, d->p[2].y)), y1 = fmaxf(d->p[0].y, fmaxf(d->p[1].y, d->p[2].y));
    mb_add((int32_t)floorf(x0), (int32_t)floorf(y0), (int32_t)ceilf(x1), (int32_t)ceilf(y1));
    if (s_meas) { for (int i = 0; i < 3; i++) { mh(d->p[i].x); mh(d->p[i].y); } mh(d->bg_opa); mh(lv_color_to_u32(d->bg_color) & 0xffffff); return; }
    lv_draw_triangle(l, d);
}

static void line_raw(float x1, float y1, float x2, float y2, float w, lv_color_t c, lv_opa_t opa)
{
    // Round the ends ourselves: LVGL 9.2 divides by zero when two float ends round to the same pixel.
    int32_t ax = lroundf(x1), ay = lroundf(y1), bx = lroundf(x2), by = lroundf(y2);
    int32_t wi = (int32_t)(w + 0.5f); if (wi < 1) wi = 1;
    if (ax == bx && ay == by) {
        lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
        r.bg_color = c; r.bg_opa = opa; r.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = { ax - wi / 2, ay - wi / 2, ax - wi / 2 + wi - 1, ay - wi / 2 + wi - 1 };
        d_rect(L, &r, &a);
        return;
    }
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1.x = ax; d.p1.y = ay; d.p2.x = bx; d.p2.y = by;
    d.width = wi;
    d.color = c; d.opa = opa; d.round_start = 1; d.round_end = 1;
    d_line(L, &d);
}
static void line_h(float x1, float y1, float x2, float y2, float w, lv_color_t c)   // head space
{
    float a, b, e, f; xf(x1, y1, &a, &b); xf(x2, y2, &e, &f);
    line_raw(a, b, e, f, w * d_S2, c, d_opa);
}
static void tri_raw(float ax, float ay, float bx, float by, float cx, float cy, lv_color_t col, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.p[0].x = ax; d.p[0].y = ay; d.p[1].x = bx; d.p[1].y = by; d.p[2].x = cx; d.p[2].y = cy;
    d.bg_color = col; d.bg_opa = opa;
    d_tri(L, &d);
}
static void tri_h(float ax, float ay, float bx, float by, float cx, float cy, lv_color_t col, lv_opa_t opa)
{
    float p[6]; xf(ax, ay, &p[0], &p[1]); xf(bx, by, &p[2], &p[3]); xf(cx, cy, &p[4], &p[5]);
    tri_raw(p[0], p[1], p[2], p[3], p[4], p[5], col, (lv_opa_t)(opa * d_opa / 255));
}
static void circle_raw(float cx, float cy, float r, lv_color_t col, lv_opa_t opa)
{
    if (r < 0.5f) return;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = col; d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { (int32_t)(cx - r), (int32_t)(cy - r), (int32_t)(cx + r), (int32_t)(cy + r) };
    d_rect(L, &d, &a);
}
// A true capsule (pill) between two screen points. LVGL's thick lines come out narrower than their
// round ends when tilted, so the eyes looked like two blobs. Here the pill is drawn upright as one
// rounded rectangle (perfect at any size); when tilted it is drawn into a small layer and that layer is
// turned as an image, smoothly, as one piece.
static void capsule_raw(float ax, float ay, float bx, float by, float w, lv_color_t c, lv_opa_t opa)
{
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (w < 2) { line_raw(ax, ay, bx, by, w, c, opa); return; }
    float cx = (ax + bx) / 2, cy = (ay + by) / 2, tl = len + w;                  // full length, end to end
    float deg = len < .5f ? 0 : atan2f(dy, dx) * 57.29578f;                     // axis angle
    float m = fmodf(fabsf(deg), 90.f); bool square = m < .3f || m > 89.7f;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.bg_opa = LV_OPA_COVER; d.radius = LV_RADIUS_CIRCLE;
    if (square) {                                                               // upright or level: draw it straight
        bool vert = fabsf(fabsf(deg) - 90.f) < 1;
        float hw = (vert ? w : tl) / 2, hh = (vert ? tl : w) / 2;
        lv_area_t a = { lroundf(cx - hw), lroundf(cy - hh), lroundf(cx + hw) - 1, lroundf(cy + hh) - 1 };
        d.bg_opa = opa; d_rect(L, &d, &a);
        return;
    }
    int lw = (int)ceilf(tl) + 4, lh = (int)ceilf(w) + 4;
    int x0 = (int)lroundf(cx) - lw / 2, y0 = (int)lroundf(cy) - lh / 2;
    lv_area_t la = { x0, y0, x0 + lw - 1, y0 + lh - 1 };
    if (s_meas) { float hd = sqrtf((float)lw * lw + (float)lh * lh) / 2;   // turned: any angle fits in this
        mb_add((int32_t)(cx - hd), (int32_t)(cy - hd), (int32_t)(cx + hd), (int32_t)(cy + hd)); mh(cx); mh(cy); mh(deg); mh(w); mh(tl); mh(opa); return; }
    lv_layer_t *ly = lv_draw_layer_create(L, LV_COLOR_FORMAT_ARGB8888, &la);
    if (!ly) { line_raw(ax, ay, bx, by, w, c, opa); return; }
    float ox = cx - lroundf(cx), oy = cy - lroundf(cy);                         // keep the sub-pixel centre
    lv_area_t ra = { lroundf(x0 + lw / 2.f + ox - tl / 2), lroundf(y0 + lh / 2.f + oy - w / 2),
                     lroundf(x0 + lw / 2.f + ox + tl / 2) - 1, lroundf(y0 + lh / 2.f + oy + w / 2) - 1 };
    lv_draw_rect(ly, &d, &ra);
    lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
    id.src = ly; id.opa = opa; id.antialias = 1;
    id.rotation = (int32_t)lroundf(deg * 10);
    id.pivot.x = lw / 2; id.pivot.y = lh / 2;
    lv_draw_layer(L, &id, &la);
}
// rounded box drawn as a capsule
static void capsule_h(float x0, float y0, float x1, float y1, lv_color_t c)
{
    float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, a, b, e, f;
    if (h >= w) { xf(cx, y0 + w / 2, &a, &b); xf(cx, y1 - w / 2, &e, &f); capsule_raw(a, b, e, f, w * d_S2, c, d_opa); }
    else        { xf(x0 + h / 2, cy, &a, &b); xf(x1 - h / 2, cy, &e, &f); capsule_raw(a, b, e, f, h * d_S2, c, d_opa); }
}
static void quad_h(float x0, float y0, float qx, float qy, float x1, float y1, float w, lv_color_t c)
{
    float px = x0, py = y0;
    for (int i = 1; i <= 8; i++) {
        float t = i / 8.f, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * qx + t * t * x1, y = u * u * y0 + 2 * u * t * qy + t * t * y1;
        line_h(px, py, x, y, w, c); px = x; py = y;
    }
}

static void eye(float cx, float cy, int side, float sx, float sy)
{
    lv_color_t W = lv_color_white(), K = lv_color_black();
    float o = CH.open, hw = 15 * sx, x0 = cx - hw, x1 = cx + hw;
    switch (CH.expr) {
    case EX_STRAIN: { float k = side < 0 ? 1 : -1;
        line_h(cx - 12 * k * sx, cy - 14 * sy, cx + 10 * k * sx, cy, 7, W);
        line_h(cx + 10 * k * sx, cy, cx - 12 * k * sx, cy + 14 * sy, 7, W); return; }
    case EX_HAPPY: quad_h(cx - 14 * sx, cy + 6, cx, cy - 18 * sy, cx + 14 * sx, cy + 6, 7, W); return;
    case EX_SLEEP: quad_h(cx - 14 * sx, cy - 2, cx, cy + 10 * sy, cx + 14 * sx, cy - 2, 6, W); return;
    case EX_SURPRISED: { float h = fmaxf(4, 48 * o * sy); capsule_h(cx - 19 * sx, cy - h / 2, cx + 19 * sx, cy + h / 2, W); return; }
    case EX_DETERMINED: case EX_SAD: {
        float h = 34 * sy, y1 = cy + h / 2, yt = cy - h / 2, drop = 13 * sy; int inner = side < 0;
        float tl, tr;
        if (CH.expr == EX_DETERMINED) { tl = inner ? yt : yt + drop; tr = inner ? yt + drop : yt; }
        else                          { tl = inner ? yt + drop : yt; tr = inner ? yt : yt + drop; }
        if (o < .4f) { capsule_h(x0, y1 - 4, x1, y1, W); return; }
        capsule_h(x0, yt, x1, y1, W);
        // cut the slanted brow with background-coloured triangles
        tri_h(x0 - 5, yt - 6, x1 + 5, yt - 6, x1 + 5, tr, K, 255);
        tri_h(x0 - 5, yt - 6, x1 + 5, tr, x0 - 5, tl, K, 255);
        return; }
    default: {
        float base = CH.expr == EX_FOCUS ? 30 : CH.expr == EX_PEEK ? 18 : 44;
        float h = fmaxf(4, base * o * sy);
        capsule_h(x0, cy - h / 2, x1, cy + h / 2, W);
    } }
}

static void star(float cx, float cy, float r, lv_opa_t opa)
{
    lv_color_t col = lv_color_hex(0xFAC775);
    float px[10], py[10];
    for (int i = 0; i < 10; i++) { float a = -(float)M_PI / 2 + i * (float)M_PI / 5, q = (i % 2) ? r * .45f : r; px[i] = cx + cosf(a) * q; py[i] = cy + sinf(a) * q; }
    for (int i = 0; i < 10; i++) tri_raw(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], col, opa);
}

bool ch_measure(lv_area_t *out, uint32_t *print)
{
    s_meas = true; s_mb[0] = s_mb[1] = INT32_MAX; s_mb[2] = s_mb[3] = INT32_MIN; s_mh = 2166136261u;
    ch_draw(NULL);
    s_meas = false;
    *print = s_mh;
    if (s_mb[0] > s_mb[2]) return false;
    out->x1 = s_mb[0]; out->y1 = s_mb[1]; out->x2 = s_mb[2]; out->y2 = s_mb[3];
    return true;
}

static void ch_draw_body(lv_layer_t *layer);
// The character is drawn from positions rounded to a fraction of a pixel. Motion stays smooth
// (anti-aliasing shows the sub-pixel steps), but while it barely moves (breathing, slow drifts)
// many frames come out identical and are not drawn or sent to the screen at all, saving power.
#ifndef CH_SNAP
#define CH_SNAP 4.f            /* steps per pixel: invisible, but most breathing frames repeat */
#endif
static float snapf(float v, float k) { return roundf(v * k) / k; }
void ch_draw(lv_layer_t *layer)
{
    if (CH.alpha < 0.02f) return;
    float sv[16] = { CH.x, CH.y, CH.gx, CH.gy, CH.scale, CH.tilt, r_lx, r_rx, r_ecy, r_cy, r_px, r_py, r_sqx, r_sqy };
    CH.x = snapf(CH.x, CH_SNAP); CH.y = snapf(CH.y, CH_SNAP); CH.gx = snapf(CH.gx, CH_SNAP); CH.gy = snapf(CH.gy, CH_SNAP);
    CH.scale = snapf(CH.scale, 256); CH.tilt = snapf(CH.tilt, 4);
    r_lx = snapf(r_lx, CH_SNAP); r_rx = snapf(r_rx, CH_SNAP); r_ecy = snapf(r_ecy, CH_SNAP); r_cy = snapf(r_cy, CH_SNAP);
    r_px = snapf(r_px, CH_SNAP); r_py = snapf(r_py, CH_SNAP); r_sqx = snapf(r_sqx, 128); r_sqy = snapf(r_sqy, 128);
    ch_draw_body(layer);
    CH.x = sv[0]; CH.y = sv[1]; CH.gx = sv[2]; CH.gy = sv[3]; CH.scale = sv[4]; CH.tilt = sv[5];
    r_lx = sv[6]; r_rx = sv[7]; r_ecy = sv[8]; r_cy = sv[9]; r_px = sv[10]; r_py = sv[11]; r_sqx = sv[12]; r_sqy = sv[13];
}

static void ch_draw_body(lv_layer_t *layer)
{
    L = layer;
    d_S2 = CH.scale; d_opa = (lv_opa_t)(CH.alpha * 255);
    float a = CH.tilt * (float)M_PI / 180; d_ca = cosf(a); d_sa = sinf(a);
    eye(r_lx, r_ecy, -1, r_sqx, r_sqy);
    eye(r_rx, r_ecy, 1, r_sqx, r_sqy);

    // umbrella: just the scalloped canopy floating over the head (no shaft or handle)
    if (CH.umb_a > .02f) {
        lv_color_t B = lv_color_hex(0x7FB2FF), K = lv_color_black();
        lv_opa_t keep = d_opa; d_opa = (lv_opa_t)(d_opa * fminf(1, CH.umb_a * 1.4f));
        float cx = CH.x, by = r_cy - UMB_BASE + umb_lift();
        float P[30][2]; int n = 0;   // 17 arc points + 3 scallops x 3
        for (int i = 0; i <= 16; i++) { float a = (float)M_PI * (1 - i / 16.f); P[n][0] = cx + UMB_RX * cosf(a); P[n++][1] = by - UMB_RY * sinf(a); }
        for (int k = 0; k < 3; k++) {                     // three scallops from right to left, bulging up
            float xa = cx + UMB_RX - k * UMB_RX * 2 / 3, xb = xa - UMB_RX * 2 / 3, xm = (xa + xb) / 2;
            for (int i = 1; i <= 3; i++) { float t = i / 3.f, u = 1 - t;
                P[n][0] = u*u*xa + 2*u*t*xm + t*t*xb; P[n++][1] = u*u*by + 2*u*t*(by - 9) + t*t*by; }
        }
        for (int i = 0; i < n; i++) tri_h(cx, by - 12, P[i][0], P[i][1], P[(i + 1) % n][0], P[(i + 1) % n][1], K, 255);
        for (int i = 0; i < n; i++) tri_h(cx, by - 12, P[i][0], P[i][1], P[(i + 1) % n][0], P[(i + 1) % n][1], B, 46);
        for (int i = 0; i < n; i++) line_h(P[i][0], P[i][1], P[(i + 1) % n][0], P[(i + 1) % n][1], 2.4f, B);
        for (int k = 1; k <= 2; k++) {                    // two ribs to the scallop points
            float xr = cx + UMB_RX - k * UMB_RX * 2 / 3;
            quad_h(cx, by - UMB_RY, xr + (cx - xr) * .15f, by - UMB_RY * .55f, xr, by, 1.4f, B);
        }
        line_h(cx, by - UMB_RY - 6, cx, by - UMB_RY, 2.6f, B);   // tip
        if (CH.snow_cap > .03f) {                         // white cap of snow along the top of the canopy
            float th = 1.5f + 4.5f * CH.snow_cap, span = .25f + .5f * fminf(1, CH.snow_cap * 1.6f);
            float px = 0, py = 0;
            for (int i = 0; i <= 12; i++) {
                float a = (float)M_PI * (.5f - span / 2 + span * i / 12.f), x = cx + (UMB_RX - 1) * cosf(a), y = by - (UMB_RY + th / 2 - 1) * sinf(a);
                if (i) line_h(px, py, x, y, th, lv_color_white());
                px = x; py = y;
            }
        }
        d_opa = keep;
    }

    // book
    if (CH.book_a > .02f) {
        lv_color_t A = lv_color_hex(0xFFB547);
        lv_opa_t fo = (lv_opa_t)(40 * CH.book_a), lo = (lv_opa_t)(255 * CH.book_a);
        float bx = CH.x + CH.gx * .35f, by = r_ecy + 32;
        lv_opa_t keep = d_opa; d_opa = (lv_opa_t)(d_opa * CH.book_a);
        for (int k = -1; k <= 1; k += 2) {
            // page outline: inner spine -> curved top -> outer edge -> curved bottom
            float P[12][2]; int n = 0;
            for (int i = 0; i <= 5; i++) { float t = i / 5.f, u = 1 - t;
                P[n][0] = u*u*(bx + k) + 2*u*t*(bx + k*13) + t*t*(bx + k*28); P[n++][1] = u*u*(by - 1) + 2*u*t*(by - 8) + t*t*(by - 6); }
            for (int i = 0; i <= 5; i++) { float t = i / 5.f, u = 1 - t;
                P[n][0] = u*u*(bx + k*28) + 2*u*t*(bx + k*13) + t*t*(bx + k); P[n++][1] = u*u*(by + 6) + 2*u*t*(by + 3) + t*t*(by + 9); }
            for (int i = 1; i < n - 1; i++) tri_h(bx + k * 14, by + 1, P[i][0], P[i][1], P[i + 1][0], P[i + 1][1], A, fo);
            for (int i = 0; i < n; i++) line_h(P[i][0], P[i][1], P[(i + 1) % n][0], P[(i + 1) % n][1], 2.2f, A);
        }
        line_h(bx, by - 1, bx, by + 10, 1.4f, A);
        if (CH.page_flip >= 0) {
            float p = CH.page_flip < .5f ? 4 * CH.page_flip * CH.page_flip * CH.page_flip : 1 - powf(-2 * CH.page_flip + 2, 3) / 2;
            float ox = 28 * cosf((float)M_PI * p), lift = sinf((float)M_PI * p) * 7;
            line_h(bx, by - 1, bx + ox, by - 6 - lift, 1.8f, A);
            line_h(bx + ox, by - 6 - lift, bx + ox, by + 6 - lift, 1.8f, A);
            line_h(bx + ox, by + 6 - lift, bx, by + 9, 1.8f, A);
        }
        (void)lo;
        d_opa = keep;
    }

    // thought bubble (screen space, beside the head)
    if (CH.think_on) {
        float e = CH.think_e;
        float out = clampf((2800 - e) / 220, 0, 1);
        float q1 = clampf(e / 180, 0, 1), q2 = clampf((e - 110) / 180, 0, 1), q3 = clampf((e - 220) / 320, 0, 1);
        #define POP(q) ((q) <= 0 ? 0 : 1 + 2.2f * powf((q) - 1, 3) + 1.2f * powf((q) - 1, 2))
        float cx = CH.think_left ? clampf(CH.bx0 - 54, 60, CH.w - 60) : clampf(CH.bx1 + 54, 60, CH.w - 60);
        float cy = snapf(clampf(CH.by0 - 4, 24, CH.h - 30) + sinf(s_now / 520) * 1.6f, CH_SNAP);
        float sx = CH.think_left ? CH.bx0 - 1 : CH.bx1 + 1, sy = CH.by0 + 10, ex = CH.think_left ? cx + 27 : cx - 27, ey = cy + 12;
        lv_color_t W = lv_color_white(), K = lv_color_black();
        circle_raw(sx + (ex - sx) * .22f, sy + (ey - sy) * .22f, 2.4f * POP(q1) * out, W, 255);
        circle_raw(sx + (ex - sx) * .62f, sy + (ey - sy) * .62f, 4 * POP(q2) * out, W, 255);
        float s = POP(q3) * out;
        if (s > 0.02f) {
            circle_raw(cx - 15 * s, cy - 1 * s, 11 * s, W, 255);
            circle_raw(cx + 2 * s, cy - 7 * s, 13 * s, W, 255);
            circle_raw(cx + 17 * s, cy, 10 * s, W, 255);
            lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.bg_color = W; r.radius = (int32_t)(8.5f * s);
            lv_area_t ar = { (int32_t)(cx - 28 * s), (int32_t)(cy - 4 * s), (int32_t)(cx + 28 * s), (int32_t)(cy + 13 * s) };
            d_rect(L, &r, &ar);
            if (!CH.think_bang) {
                for (int i = 0; i < 3; i++) {
                    float ph = fmodf((e - 540) / 900 - i * .18f + 10, 1.f);
                    float hop = (ph > 0 && ph < .3f) ? -sinf(ph / .3f * (float)M_PI) * 3 : 0;
                    circle_raw(cx + (i - 1) * 10 * s, cy + (3 + hop) * s, 2.6f * s, K, 255);
                }
            } else {
                lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
                ld.font = &font_bold18; ld.color = K; ld.text = "!"; ld.align = LV_TEXT_ALIGN_CENTER;
                lv_area_t la = { (int32_t)(cx - 10), (int32_t)(cy - 10), (int32_t)(cx + 10), (int32_t)(cy + 14) };
                d_label(L, &ld, &la);
            }
        }
        #undef POP
    }

    // zzz
    if (CH.zzz) {
        for (int i = 0; i < 3; i++) {
            float q = fmodf(s_now / 2400 + i / 3.f, 1.f);
            lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
            ld.font = &font_bold18; ld.color = lv_color_hex(0xCFCFCF); ld.text = i == 2 ? "Z" : "z";
            ld.opa = (lv_opa_t)((int)(sinf(q * (float)M_PI) * 230 * CH.alpha) & ~7);   // 8-step fade: fewer repaints
            float x = fminf(CH.w - 16, CH.x + 48 * CH.scale + q * 26 + i * 2), y = fmaxf(4, r_cy - 24 * CH.scale - q * 40) - 14;
            lv_area_t la = { (int32_t)x, (int32_t)y, (int32_t)x + 20, (int32_t)y + 24 };
            d_label(L, &ld, &la);
        }
    }

    // star burst over the head
    float q = (s_now - CH.stars_t) / 700;
    if (q >= 0 && q < 1) {
        float hx = (CH.bx0 + CH.bx1) / 2, hy = CH.by0;
        for (int i = 0; i < 5; i++) {
            float an = -(float)M_PI / 2 + (i - 2) * .7f;
            star(hx + cosf(an) * (20 + 50 * q) * CH.scale, hy + 10 + sinf(an) * (20 + 40 * q) * CH.scale, (7 - 3 * q) * fmaxf(.7f, CH.scale), (lv_opa_t)((1 - q) * 255));
        }
    }
}
