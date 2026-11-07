// Vector icons for the device UI (see icons.h). Drawn with LVGL lines, arcs, rectangles and
// triangles; every coordinate below is in a unit box from -0.5 to 0.5, y pointing down.
#include "icons.h"
#include <math.h>
#include <stdlib.h>

static lv_layer_t *L;
static float CX, CY, S, SW;
static lv_color_t COL;
static lv_opa_t OPA;

#define X(v) (CX + (v) * S)
#define Y(v) (CY + (v) * S)

static void line(float x1, float y1, float x2, float y2)
{
    int32_t ax = lroundf(X(x1)), ay = lroundf(Y(y1)), bx = lroundf(X(x2)), by = lroundf(Y(y2));
    if (ax == bx && ay == by) return;                         // LVGL 9.2 zero-length line bug
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1.x = ax; d.p1.y = ay; d.p2.x = bx; d.p2.y = by;
    d.width = (int32_t)(SW + .5f); d.color = COL; d.opa = OPA; d.round_start = d.round_end = 1;
    lv_draw_line(L, &d);
}
static void arc(float x, float y, float r, int a0, int a1)       // degrees, 0 = right, clockwise
{
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x = lroundf(X(x)); d.center.y = lroundf(Y(y)); d.radius = (uint16_t)lroundf(r * S + SW / 2);
    d.start_angle = a0; d.end_angle = a1; d.width = (int32_t)(SW + .5f); d.color = COL; d.opa = OPA; d.rounded = 1;
    lv_draw_arc(L, &d);
}
static void rect(float x0, float y0, float x1, float y1, float rad, bool fill)
{
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = (int32_t)(rad * S);
    if (fill) { d.bg_color = COL; d.bg_opa = OPA; }
    else { d.bg_opa = 0; d.border_color = COL; d.border_opa = OPA; d.border_width = (int32_t)(SW + .5f); }
    lv_area_t a = { lroundf(X(x0)), lroundf(Y(y0)), lroundf(X(x1)), lroundf(Y(y1)) };
    lv_draw_rect(L, &d, &a);
}
static void dot(float x, float y, float r) { rect(x - r, y - r, x + r, y + r, r, true); }
static void tri(float ax, float ay, float bx, float by, float cx, float cy)
{
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.p[0].x = lroundf(X(ax)); d.p[0].y = lroundf(Y(ay)); d.p[1].x = lroundf(X(bx)); d.p[1].y = lroundf(Y(by));   // rounded, not truncated:
    d.p[2].x = lroundf(X(cx)); d.p[2].y = lroundf(Y(cy));                                                         // mirrored shapes stay mirrored
    d.bg_color = COL; d.bg_opa = OPA;
    lv_draw_triangle(L, &d);
}
// arrowhead at point (x,y) pointing along angle a (radians)
static void head(float x, float y, float a, float len)
{
    float l = len, w = .55f;
    tri(x + cosf(a) * l * .45f, y + sinf(a) * l * .45f,
        x - cosf(a) * l * .55f + cosf(a + 1.5708f) * l * w, y - sinf(a) * l * .55f + sinf(a + 1.5708f) * l * w,
        x - cosf(a) * l * .55f - cosf(a + 1.5708f) * l * w, y - sinf(a) * l * .55f - sinf(a + 1.5708f) * l * w);
}
// One straight stroke with round ends, drawn as a true capsule. LVGL draws a tilted thick line a
// little narrower than its round ends, so where two strokes meet (the corner of a tick) the end of
// one poked out of the other. Here the stroke is a rounded rectangle drawn level in a small layer and
// turned into place as one piece, so two strokes sharing an end point meet in a clean round corner.
static void stroke(float x1, float y1, float x2, float y2)
{
    float ax = X(x1), ay = Y(y1), bx = X(x2), by = Y(y2), dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy), w = SW, tl = len + w, cx = (ax + bx) / 2, cy = (ay + by) / 2;
    if (OPA < 255 || len < 1) { line(x1, y1, x2, y2); return; }
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = COL; d.bg_opa = LV_OPA_COVER; d.radius = LV_RADIUS_CIRCLE;
    int lw = (int)ceilf(tl) + 4, lh = (int)ceilf(w) + 4;
    int x0 = (int)lroundf(cx) - lw / 2, y0 = (int)lroundf(cy) - lh / 2;
    lv_area_t la = { x0, y0, x0 + lw - 1, y0 + lh - 1 };
    lv_layer_t *ly = lv_draw_layer_create(L, LV_COLOR_FORMAT_ARGB8888, &la);
    if (!ly) { line(x1, y1, x2, y2); return; }
    float ox = cx - lroundf(cx), oy = cy - lroundf(cy);                 // keep the sub-pixel centre
    lv_area_t ra = { lroundf(x0 + lw / 2.f + ox - tl / 2), lroundf(y0 + lh / 2.f + oy - w / 2),
                     lroundf(x0 + lw / 2.f + ox + tl / 2) - 1, lroundf(y0 + lh / 2.f + oy + w / 2) - 1 };
    lv_draw_rect(ly, &d, &ra);
    lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
    id.src = ly; id.opa = OPA; id.antialias = 1;
    id.rotation = (int32_t)lroundf(atan2f(dy, dx) * 572.9578f);           // 0.1 degree units
    id.pivot.x = lw / 2; id.pivot.y = lh / 2;
    lv_draw_layer(L, &id, &la);
}
// Some shapes are rendered exactly, pixel by pixel, from their distance to the outline: a tick (a
// two-stroke line with a round corner) and the star (a filled outline). LVGL builds these from
// separate lines or triangles, which left a nub at the tick's corner and faint seams inside the star.
// Each distinct shape (size, colour, sub-pixel position) is made once and kept.
typedef struct { int w, h; uint32_t key; lv_draw_buf_t *buf; lv_image_dsc_t img; } shape_t;
static shape_t s_shape[32]; static int s_nshape;
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay, l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}
// pts: n points in screen space. fill = false: an open line of radius r; true: a filled closed outline.
static bool shape_draw(const float (*pts)[2], int n, float r, bool fill)
{
    float mx = 1e9f, my = 1e9f, Mx = -1e9f, My = -1e9f, pad = (fill ? 0 : r) + 1;
    for (int i = 0; i < n; i++) { mx = fminf(mx, pts[i][0]); my = fminf(my, pts[i][1]); Mx = fmaxf(Mx, pts[i][0]); My = fmaxf(My, pts[i][1]); }
    int x0 = (int)floorf(mx - pad), y0 = (int)floorf(my - pad), w = (int)ceilf(Mx + pad) - x0, h = (int)ceilf(My + pad) - y0;
    if (w <= 0 || h <= 0 || w * h > 160 * 160) return false;
    float q[12][2]; if (n > 12) return false;
    for (int i = 0; i < n; i++) { q[i][0] = pts[i][0] - x0; q[i][1] = pts[i][1] - y0; }
    uint32_t key = 2166136261u;
    #define MIX(v) key = (key ^ (uint32_t)(v)) * 16777619u
    for (int i = 0; i < n; i++) { MIX(lroundf(q[i][0] * 8)); MIX(lroundf(q[i][1] * 8)); }
    MIX(lroundf(r * 8)); MIX(lv_color_to_u32(COL)); MIX(w); MIX(h); MIX(fill); MIX(n);
    #undef MIX
    shape_t *sh = NULL;
    for (int i = 0; i < s_nshape; i++) if (s_shape[i].key == key && s_shape[i].w == w && s_shape[i].h == h) { sh = &s_shape[i]; break; }
    if (!sh) {
        if (s_nshape >= (int)(sizeof s_shape / sizeof s_shape[0])) return false;
        lv_draw_buf_t *b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if (!b) return false;
        lv_color32_t c32 = lv_color_to_32(COL, 255);
        for (int y = 0; y < h; y++) {
            lv_color32_t *row = (lv_color32_t *)(b->data + y * b->header.stride);
            for (int x = 0; x < w; x++) {
                float px = x + .5f, py = y + .5f, d = 1e9f, a;
                int segs = fill ? n : n - 1;
                for (int k = 0; k < segs; k++) d = fminf(d, seg_dist(px, py, q[k][0], q[k][1], q[(k + 1) % n][0], q[(k + 1) % n][1]));
                if (fill) {
                    bool in = false;                       // even-odd inside test
                    for (int k = 0, m = n - 1; k < n; m = k++)
                        if ((q[k][1] > py) != (q[m][1] > py) && px < (q[m][0] - q[k][0]) * (py - q[k][1]) / (q[m][1] - q[k][1]) + q[k][0]) in = !in;
                    a = in ? .5f + d : .5f - d;
                } else a = r - d + .5f;
                a = a < 0 ? 0 : a > 1 ? 1 : a;
                row[x] = c32; row[x].alpha = (uint8_t)lroundf(a * 255);
            }
        }
        sh = &s_shape[s_nshape++];
        sh->key = key; sh->w = w; sh->h = h; sh->buf = b;
        lv_draw_buf_to_image(b, &sh->img);
    }
    lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
    id.src = &sh->img; id.opa = OPA;
    lv_area_t a = { x0, y0, x0 + w - 1, y0 + h - 1 };
    lv_draw_image(L, &id, &a);
    return true;
}
static void check(float sc, float ox, float oy)
{
    float P[3][2] = { { X(ox - .34f * sc), Y(oy + .02f * sc) }, { X(ox - .1f * sc), Y(oy + .26f * sc) }, { X(ox + .36f * sc), Y(oy - .26f * sc) } };
    if (shape_draw((const float (*)[2])P, 3, SW / 2, false)) return;
    stroke(ox - .34f * sc, oy + .02f * sc, ox - .1f * sc, oy + .26f * sc);     // (cache full or too big)
    stroke(ox - .1f * sc, oy + .26f * sc, ox + .36f * sc, oy - .26f * sc);
}

// A curved arrow: clockwise arc from a0 to a1 (degrees) with its head sitting on the circle itself:
// the tip lies further along the same circle and the base runs straight across the line, so head and
// line share one centre line at every size.
static void arc_arrow(float cx, float cy, float r, int a0, int a1, float hw, float tip_len)
{
    const float D2R = 3.14159265f / 180;
    arc(cx, cy, r, a0, a1);
    float e = a1 * D2R, t = e + tip_len / r;
    tri(cx + cosf(t) * r, cy + sinf(t) * r,
        cx + cosf(e) * (r - hw), cy + sinf(e) * (r - hw),
        cx + cosf(e) * (r + hw), cy + sinf(e) * (r + hw));
}

void icon_draw(lv_layer_t *layer, icon_t id, float cx, float cy, float size, lv_color_t col, lv_opa_t opa)
{
    L = layer; CX = cx; CY = cy; S = size; COL = col; OPA = opa;
    SW = fmaxf(1.6f, size * .1f);
    const float D2R = 3.14159f / 180;
    switch (id) {
    case IC_FLIP:
        CX = roundf(CX); CY = roundf(CY);
        arc_arrow(0, 0, .34f, 200, 318, .11f, .17f);
        arc_arrow(0, 0, .34f, 20, 138, .11f, .17f);
        break;
    case IC_BOOK:
        line(0, -.26f, 0, .34f);
        line(0, -.26f, -.44f, -.34f); line(-.44f, -.34f, -.44f, .26f); line(-.44f, .26f, 0, .34f);
        line(0, -.26f, .44f, -.34f);  line(.44f, -.34f, .44f, .26f);  line(.44f, .26f, 0, .34f);
        break;
    case IC_CUP:
        line(-.36f, -.1f, .22f, -.1f);
        line(-.34f, -.1f, -.28f, .34f); line(-.28f, .34f, .14f, .34f); line(.14f, .34f, .2f, -.1f);
        arc(.24f, .08f, .11f, 270, 90);
        line(-.16f, -.44f, -.16f, -.26f); line(.04f, -.44f, .04f, -.26f);
        break;
    case IC_PAUSE: rect(-.3f, -.36f, -.08f, .36f, .06f, true); rect(.08f, -.36f, .3f, .36f, .06f, true); break;
    case IC_PLAY:  tri(-.26f, -.38f, .38f, 0, -.26f, .38f); break;
    case IC_STOP:  rect(-.3f, -.3f, .3f, .3f, .08f, true); break;
    case IC_TAP:
        dot(0, .12f, .15f);
        line(0, -.2f, 0, -.42f); line(-.22f, -.12f, -.38f, -.28f); line(.22f, -.12f, .38f, -.28f);
        break;
    case IC_HOLD: dot(0, 0, .15f); arc(0, 0, .36f, 0, 360); break;
    case IC_CHECK: check(1.1f, 0, 0); break;
    case IC_DONE:  arc(0, 0, .42f, 0, 360); check(.7f, 0, 0); break;
    case IC_DONE_FLIP: {
        // two identical arrows: the second is the first turned 180 degrees about a whole-pixel centre.
        // Each head sits on the arc itself (tip further along the circle, base square across the line).
        CX = roundf(CX); CY = roundf(CY);
        const float r = .40f, hw = .12f;
        const int a0 = 200, a1 = 318, tip = 22;
        for (int k = 0; k < 2; k++) {
            int o = k * 180;
            arc(0, 0, r, a0 + o, a1 + o);
            float e = (a1 + o) * D2R, t = (a1 + o + tip) * D2R;
            tri(cosf(t) * r, sinf(t) * r, cosf(e) * (r - hw), sinf(e) * (r - hw), cosf(e) * (r + hw), sinf(e) * (r + hw));
        }
        SW = fmaxf(1.6f, S * .08f);            // a slightly finer tick, centred, clear of both heads
        check(.46f, -.01f, 0);
        break;
    }
    case IC_TASKS:
        rect(-.44f, -.36f, -.16f, -.08f, .04f, false); line(-.02f, -.22f, .44f, -.22f);
        rect(-.44f, .08f, -.16f, .36f, .04f, false);   line(-.02f, .22f, .44f, .22f);
        break;
    case IC_SWEEP:
        rect(-.44f, -.36f, -.16f, -.08f, .04f, false); line(-.02f, -.22f, .44f, -.22f);
        rect(-.44f, .08f, -.16f, .36f, .04f, false);   line(-.02f, .22f, .44f, .22f);
        line(-.48f, .46f, .48f, -.46f);
        break;
    case IC_PHONE: rect(-.24f, -.44f, .24f, .44f, .08f, false); line(-.06f, .32f, .06f, .32f); break;
    case IC_WIFI:
        arc(0, .3f, .2f, 225, 315); arc(0, .3f, .4f, 225, 315); arc(0, .3f, .6f, 228, 312);
        dot(0, .3f, .07f);
        break;
    case IC_LOCK:
        rect(-.32f, -.04f, .32f, .42f, .06f, true);
        arc(0, -.12f, .2f, 180, 360); line(-.2f, -.12f, -.2f, -.04f); line(.2f, -.12f, .2f, -.04f);
        break;
    case IC_SUN:
        dot(0, 0, .17f);
        for (int i = 0; i < 8; i++) { float a = i * 45 * D2R; line(cosf(a) * .3f, sinf(a) * .3f, cosf(a) * .44f, sinf(a) * .44f); }
        break;
    case IC_ROTATE:
        rect(-.14f, -.26f, .14f, .26f, .05f, false);
        arc_arrow(0, 0, .44f, 200, 328, .11f, .17f);
        break;
    case IC_RESET:
        arc_arrow(0, .02f, .34f, 300, 200, .12f, .17f);
        break;
    case IC_CROSS: line(-.3f, -.3f, .3f, .3f); line(.3f, -.3f, -.3f, .3f); break;
    case IC_UMBRELLA:
        arc(0, .04f, .44f, 180, 360); line(-.44f, .04f, .44f, .04f);
        line(0, .04f, 0, .36f); arc(-.09f, .36f, .09f, 0, 180);
        break;
    case IC_SNOW:
        for (int i = 0; i < 3; i++) {
            float a = (90 + i * 60) * D2R, ca = cosf(a), sa = sinf(a);
            line(-ca * .44f, -sa * .44f, ca * .44f, sa * .44f);
            for (int e = -1; e <= 1; e += 2) {                  // little V at both ends
                float ex = ca * .3f * e, ey = sa * .3f * e, nx = -sa * .1f, ny = ca * .1f;
                line(ex, ey, ex + ca * .12f * e + nx, ey + sa * .12f * e + ny);
                line(ex, ey, ex + ca * .12f * e - nx, ey + sa * .12f * e - ny);
            }
        }
        break;
    case IC_PLUG:
        line(-.12f, -.44f, -.12f, -.22f); line(.12f, -.44f, .12f, -.22f);
        rect(-.26f, -.22f, .26f, .12f, .08f, true); line(0, .12f, 0, .44f);
        break;
    case IC_POWER: arc(0, .04f, .36f, 300, 240); line(0, -.44f, 0, -.04f); break;
    case IC_BATT_LOW:
        rect(-.44f, -.2f, .34f, .2f, .06f, false); rect(.36f, -.08f, .46f, .08f, .03f, true);
        rect(-.34f, -.1f, -.22f, .1f, .02f, true);
        break;
    case IC_HOME:
        line(-.44f, -.02f, 0, -.4f); line(0, -.4f, .44f, -.02f);
        line(-.3f, -.12f, -.3f, .38f); line(-.3f, .38f, .3f, .38f); line(.3f, .38f, .3f, -.12f);
        break;
    case IC_CHEV_L: line(.14f, -.3f, -.14f, 0); line(-.14f, 0, .14f, .3f); break;
    case IC_CHEV_R: line(-.14f, -.3f, .14f, 0); line(.14f, 0, -.14f, .3f); break;
    case IC_SKIP:
        line(-.34f, -.28f, -.06f, 0); line(-.06f, 0, -.34f, .28f);
        line(.04f, -.28f, .32f, 0);  line(.32f, 0, .04f, .28f);
        break;
    case IC_UP: line(0, .42f, 0, -.3f); head(0, -.3f, -1.5708f, .34f); break;
    case IC_STAR: {
        float px[10], py[10];
        for (int i = 0; i < 10; i++) { float a = -1.5708f + i * 3.14159f / 5, q = (i % 2) ? .2f : .46f; px[i] = cosf(a) * q; py[i] = sinf(a) * q + .04f; }
        float P[10][2]; for (int i = 0; i < 10; i++) { P[i][0] = X(px[i]); P[i][1] = Y(py[i]); }
        if (!shape_draw((const float (*)[2])P, 10, 0, true))
            for (int i = 0; i < 10; i++) tri(0, .04f, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10]);
        break; }
    case IC_PLUS:  SW = 2 * roundf(SW / 2); line(-.34f, 0, .34f, 0); line(0, -.34f, 0, .34f); break;   // even stroke: centred exactly
    case IC_MINUS: SW = 2 * roundf(SW / 2); line(-.34f, 0, .34f, 0); break;
    case IC_KNOCK: dot(-.42f, 0, .1f); dot(-.14f, 0, .1f); dot(.38f, 0, .1f); break;
    default: break;
    }
}

// ---------------------------------------------------------------- icon objects
typedef struct { icon_t id; uint32_t color; int size; } icon_ud_t;

static void icon_ev(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    icon_ud_t *u = lv_obj_get_user_data(o);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) { free(u); return; }
    if (!u || u->id == IC_NONE) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    icon_draw(lv_event_get_layer(e), u->id, (a.x1 + a.x2 + 1) / 2.f, (a.y1 + a.y2 + 1) / 2.f, u->size, lv_color_hex(u->color),   // true middle of the box
              lv_obj_get_style_opa_recursive(o, 0));
}

lv_obj_t *icon_create(lv_obj_t *parent, icon_t id, int cx, int cy, int size, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    int pad = size / 6 + 2;                        // room for round line caps
    lv_obj_set_size(o, size + pad * 2, size + pad * 2);
    lv_obj_set_pos(o, cx - size / 2 - pad, cy - size / 2 - pad);
    icon_ud_t *u = malloc(sizeof *u); u->id = id; u->color = color; u->size = size;
    lv_obj_set_user_data(o, u);
    lv_obj_add_event_cb(o, icon_ev, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, icon_ev, LV_EVENT_DELETE, NULL);
    return o;
}

void icon_set(lv_obj_t *o, icon_t id) { icon_ud_t *u = lv_obj_get_user_data(o); if (u && u->id != id) { u->id = id; lv_obj_invalidate(o); } }
void icon_set_color(lv_obj_t *o, uint32_t c) { icon_ud_t *u = lv_obj_get_user_data(o); if (u && u->color != c) { u->color = c; lv_obj_invalidate(o); } }
