// Screens. Layouts and motion follow the Chibi simulator (640x172 landscape, 172x640 portrait).
#include "ui.h"
#include "character.h"
#include "store.h"
#include "power.h"
#include "net.h"
#include "rtc.h"
#include "display.h"
#include "weather.h"
#include "icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_log.h"

LV_FONT_DECLARE(font_num92); LV_FONT_DECLARE(font_num72); LV_FONT_DECLARE(font_num60);
LV_FONT_DECLARE(font_num26); LV_FONT_DECLARE(font_num22); LV_FONT_DECLARE(font_num28); LV_FONT_DECLARE(font_num36); LV_FONT_DECLARE(font_num150); LV_FONT_DECLARE(font_num132); LV_FONT_DECLARE(font_mono12);
LV_FONT_DECLARE(font_sans11); LV_FONT_DECLARE(font_sans16); LV_FONT_DECLARE(font_title30);
LV_FONT_DECLARE(font_bold18); LV_FONT_DECLARE(font_jp13); LV_FONT_DECLARE(font_jp16); LV_FONT_DECLARE(font_jp19); LV_FONT_DECLARE(font_jp22);

#define C_AMBER 0xFFB547
#define C_SKY   0x7FB2FF
#define C_MINT  0x7EE0A0
#define C_ORANGE 0xFF9F5A
#define C_RED   0xFF7A7A
#define SPD 0.65f
// When a digit changes, the old one is knocked off and tumbles away (0) or fades where it stood (1).
#ifndef DIGIT_GHOST_FADE
#define DIGIT_GHOST_FADE 0
#endif

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
// Style setters invalidate (repaint) their object even when the value is unchanged, so per-frame
// updates go through these and touch the object only when something actually changed.
static void set_opa(lv_obj_t *o, lv_opa_t v) { if (lv_obj_get_style_opa(o, 0) != v) lv_obj_set_style_opa(o, v, 0); }
static void set_scale(lv_obj_t *o, int32_t sx, int32_t sy)
{
    if (abs(sx - 256) <= 5 && abs(sy - 256) <= 5) sx = sy = 256;     // nearly settled (<2%): draw plainly, no transform
    if (lv_obj_get_style_transform_scale_x(o, 0) != sx) lv_obj_set_style_transform_scale_x(o, sx, 0);
    if (lv_obj_get_style_transform_scale_y(o, 0) != sy) lv_obj_set_style_transform_scale_y(o, sy, 0);
}
static void set_hidden(lv_obj_t *o, bool h) { if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != h) { if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); } }
static void set_rot(lv_obj_t *o, int32_t r) { if (lv_obj_get_style_transform_rotation(o, 0) != r) lv_obj_set_style_transform_rotation(o, r, 0); }

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float easeOut(float p) { return 1 - powf(1 - p, 3); }

static lv_obj_t *s_page, *s_overlay, *s_msg;
static screen_t s_scr;
static int W = 640, H = 172;
static float s_t0, s_now, s_acc;
static int64_t s_last_us;
static uint32_t s_seen_ver;
static float s_knock_t = -1e9f, s_flash_t = -1e9f, s_shake_until, s_shake_amp;

// ================================================================== labels
static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, const lv_font_t *f, uint32_t col, int x, int base, int align)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_update_layout(l);
    int w = lv_obj_get_width(l);
    int top = base - (f->line_height - f->base_line);
    lv_obj_set_pos(l, align == 0 ? x : align == 1 ? x - w / 2 : x - w, top);
    lv_obj_set_user_data(l, (void *)(intptr_t)((x << 2) | align));
    return l;
}
static void relabel(lv_obj_t *l, const char *txt)
{
    if (!l || !strcmp(lv_label_get_text(l), txt)) return;
    lv_label_set_text(l, txt);
    intptr_t ud = (intptr_t)lv_obj_get_user_data(l);
    int x = (int)(ud >> 2), align = (int)(ud & 3);
    lv_obj_update_layout(l);
    int w = lv_obj_get_width(l);
    lv_obj_set_x(l, align == 0 ? x : align == 1 ? x - w / 2 : x - w);
}
static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t col, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// ================================================================== icon rows
// A row of up to 4 items, each an icon with an optional number/short text after it, laid out
// left, centre or right of an anchor. Used for every hint and status line instead of words.
#define IR_MAX 4
typedef struct { icon_t ic; const char *tx; uint32_t col; } iitem_t;
typedef struct { lv_obj_t *ic[IR_MAX], *tx[IR_MAX]; int x, base, align, isz; const lv_font_t *f; char sig[96]; } irow_t;

static void irow_make(irow_t *r, int x, int base, int align, int isz, const lv_font_t *f)
{
    memset(r, 0, sizeof *r);
    r->x = x; r->base = base; r->align = align; r->isz = isz; r->f = f;
    for (int i = 0; i < IR_MAX; i++) {
        r->ic[i] = icon_create(s_page, IC_NONE, 0, 0, isz, 0xFFFFFF);
        r->tx[i] = lv_label_create(s_page);
        lv_obj_set_style_text_font(r->tx[i], f, 0);
        lv_label_set_text(r->tx[i], "");
        lv_obj_add_flag(r->ic[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(r->tx[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void irow_set(irow_t *r, int n, const iitem_t *it)
{
    char sig[96]; int o = 0;
    for (int i = 0; i < n && o < 90; i++) o += snprintf(sig + o, sizeof sig - o, "%d%s%lx|", it[i].ic, it[i].tx ? it[i].tx : "", (unsigned long)it[i].col);
    sig[o] = 0;
    if (!strcmp(sig, r->sig)) return;
    snprintf(r->sig, sizeof r->sig, "%s", sig);
    int w[IR_MAX] = { 0 }, total = 0;
    const int gap_in = 4, gap_out = 12;
    for (int i = 0; i < IR_MAX; i++) {
        bool on = i < n;
        bool has_tx = on && it[i].tx && it[i].tx[0];
        if (!on || it[i].ic == IC_NONE) lv_obj_add_flag(r->ic[i], LV_OBJ_FLAG_HIDDEN);
        if (!has_tx) lv_obj_add_flag(r->tx[i], LV_OBJ_FLAG_HIDDEN);
        if (!on) continue;
        if (has_tx) {
            lv_label_set_text(r->tx[i], it[i].tx);
            lv_obj_set_style_text_color(r->tx[i], lv_color_hex(it[i].col), 0);
            lv_obj_update_layout(r->tx[i]);
            w[i] = lv_obj_get_width(r->tx[i]);
        }
        int iw = it[i].ic != IC_NONE ? r->isz : 0;
        total += iw + (iw && w[i] ? gap_in : 0) + w[i] + (i < n - 1 ? gap_out : 0);
    }
    int x = r->align == 0 ? r->x : r->align == 1 ? r->x - total / 2 : r->x - total;
    int icy = r->base - (int)(r->f->line_height - r->f->base_line) / 2 + 1;   // icons centred on the text
    for (int i = 0; i < n; i++) {
        if (it[i].ic != IC_NONE) {
            icon_set(r->ic[i], it[i].ic); icon_set_color(r->ic[i], it[i].col);
            int pw = lv_obj_get_width(r->ic[i]);
            lv_obj_set_pos(r->ic[i], x + r->isz / 2 - pw / 2, icy - pw / 2);
            lv_obj_remove_flag(r->ic[i], LV_OBJ_FLAG_HIDDEN);
            x += r->isz + (w[i] ? gap_in : 0);
        }
        if (w[i]) {
            lv_obj_set_pos(r->tx[i], x, r->base - (r->f->line_height - r->f->base_line));
            lv_obj_remove_flag(r->tx[i], LV_OBJ_FLAG_HIDDEN);
            x += w[i];
        }
        x += gap_out;
    }
}
static irow_t IR1, IR2, IR3;

// ================================================================== squishy digits
typedef struct {
    lv_obj_t *l; const lv_font_t *f; char ch[2]; uint32_t col; lv_area_t box; bool box_ok;
    float x, y, hx, hy, vx, vy, k, c, sx, sv, rip, ripA; bool land, fall; float drop_at; int w, h, yoff;
} digit_t;
static digit_t D[6]; static int ND;
static struct { lv_obj_t *l; bool on; float x, y, vx, vy, rot, vr, a; } G;

// The big digits are drawn once per character into a picture and reused. Squashing a text label
// made LVGL redraw the text into a scratch layer and then stretch that layer every frame, for each
// digit; stretching a ready picture is several times cheaper on the chip.
// Kept as alpha-only (A8) pictures, coloured when drawn: a quarter of the memory traffic of full
// colour, which is what stretching and turning them costs on the chip.
static struct { const lv_font_t *f; char c; lv_draw_buf_t *buf; } s_gc[40];
static const lv_image_dsc_t *glyph_img(const lv_font_t *f, char c, uint32_t col)
{
    (void)col;
    for (int i = 0; i < 40; i++) if (s_gc[i].buf && s_gc[i].f == f && s_gc[i].c == c) return (const lv_image_dsc_t *)s_gc[i].buf;
    int slot = -1;
    for (int i = 0; i < 40; i++) if (!s_gc[i].buf) { slot = i; break; }
    if (slot < 0) { slot = rand() % 40; lv_draw_buf_destroy(s_gc[slot].buf); s_gc[slot].buf = NULL; }
    char t[2] = { c, 0 };
    lv_point_t sz; lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x < 1) sz.x = 1;
    lv_draw_buf_t *b = lv_draw_buf_create(sz.x, sz.y, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!b) return NULL;
    lv_draw_buf_clear(b, NULL);
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, b);
    lv_layer_t ly; lv_canvas_init_layer(cv, &ly);
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = lv_color_white(); d.text = t; d.text_local = 1;
    lv_area_t a = { 0, 0, sz.x - 1, sz.y - 1 };
    lv_draw_label(&ly, &d, &a);
    lv_canvas_finish_layer(cv, &ly);
    lv_obj_delete(cv);
    lv_draw_buf_t *m = lv_draw_buf_create(sz.x, sz.y, LV_COLOR_FORMAT_A8, 0);      // keep only the coverage
    if (!m) { lv_draw_buf_destroy(b); return NULL; }
    for (int y = 0; y < sz.y; y++) {
        const uint8_t *src = b->data + y * b->header.stride; uint8_t *dst = m->data + y * m->header.stride;
        for (int x = 0; x < sz.x; x++) dst[x] = src[x * 4 + 3];
    }
    lv_draw_buf_destroy(b);
    s_gc[slot].f = f; s_gc[slot].c = c; s_gc[slot].buf = m;
    return (const lv_image_dsc_t *)m;
}
static void digit_char(digit_t *d, char c)
{
    d->ch[0] = c; lv_image_set_src(d->l, glyph_img(d->f, c, d->col));
    lv_obj_set_style_image_recolor(d->l, lv_color_hex(d->col), 0); lv_obj_set_style_image_recolor_opa(d->l, LV_OPA_COVER, 0);
}
static void img_scale(lv_obj_t *o, int32_t sx, int32_t sy)
{
    if (abs(sx - 256) <= 5 && abs(sy - 256) <= 5) sx = sy = 256;     // nearly settled (<2%): drawn plainly, no stretching
    if ((int32_t)lv_image_get_scale_x(o) != sx) lv_image_set_scale_x(o, sx);
    if ((int32_t)lv_image_get_scale_y(o) != sy) lv_image_set_scale_y(o, sy);
}

static void digit_place(digit_t *d)
{
    lv_obj_set_pos(d->l, (int)(d->x - d->w / 2.f), (int)(d->y - d->yoff));
}

static void make_digits(const char *txt, const lv_font_t *f, float cx, float cy, uint32_t col)
{
    ND = 0;
    int n = strlen(txt);
    lv_font_glyph_dsc_t g; lv_font_get_glyph_dsc(f, &g, '0', 0);
    bool big = f == &font_num150 || f == &font_num132;   // digit-only fonts: tight colon, centre on the glyph box
    float adv = g.adv_w, cadv = big ? adv * .5f : adv, total = 0;
    for (int i = 0; i < n && i < 6; i++) total += txt[i] == ':' ? cadv : adv;
    float x = cx - total / 2;
    int size = f->line_height - f->base_line;          // ~ascent
    for (int i = 0; i < n && i < 6; i++) {
        float a = txt[i] == ':' ? cadv : adv; x += a / 2;
        digit_t *d = &D[ND++];
        memset(d, 0, sizeof *d);
        d->f = f; d->col = col;
        d->l = lv_image_create(s_page);
        digit_char(d, txt[i]);
        lv_obj_update_layout(d->l);
        d->w = lv_obj_get_width(d->l); d->h = lv_obj_get_height(d->l);
        float em = f->line_height / 1.32f;                     // JetBrains Mono line height is 1.32 em
        d->yoff = (int)(size - 0.365f * em);                     // centre of the digit glyphs (cap height 0.73 em)
        if (big) {                                                // digits-only font: centre on the real glyph box
            lv_font_glyph_dsc_t gg; lv_font_get_glyph_dsc(f, &gg, '0', 0);
            d->yoff = size - (gg.ofs_y + gg.box_h / 2);
        }
        d->x = d->hx = x; d->y = d->hy = cy; d->k = .05f; d->c = .3f; d->sx = 1;
        lv_image_set_pivot(d->l, d->w / 2, d->h - f->base_line);
        digit_place(d);
        x += a / 2;
    }
    G.l = lv_image_create(s_page);
    lv_obj_add_flag(G.l, LV_OBJ_FLAG_HIDDEN);
    G.on = false;
}

static void ripple(int gi, float amp)
{
    for (int i = 0; i < ND; i++) { if (i == gi) continue; int dist = abs(i - gi); D[i].rip = s_now + dist * 70; D[i].ripA = amp * powf(.65f, dist); }
}

static void digit_roll(int i, char c, float delay)
{
    digit_t *d = &D[i];
    if (d->ch[0] == c) return;
    G.on = true; lv_image_set_src(G.l, glyph_img(d->f, d->ch[0], d->col)); lv_obj_set_style_image_recolor(G.l, lv_color_hex(d->col), 0); lv_obj_set_style_image_recolor_opa(G.l, LV_OPA_COVER, 0); set_hidden(G.l, false); G.x = d->x; G.y = d->y; G.rot = 0; G.a = 1;
#if DIGIT_GHOST_FADE
    G.vx = 0; G.vy = -.5f * SPD; G.vr = 1.2f * SPD;           // fades where it stood
#else
    G.vx = 1.1f * SPD; G.vy = -2.6f * SPD; G.vr = 4 * SPD;    // the old digit is knocked off and tumbles away
#endif
    lv_image_set_pivot(G.l, d->w / 2, d->h / 2);
    digit_char(d, c);
    d->y = -d->h; d->vx = d->vy = 0; d->fall = true; d->drop_at = s_now + delay;
}

static void digits_set(const char *txt, bool roll)
{
    float delay = 350;
    for (int i = 0; i < ND && txt[i]; i++) {
        if (D[i].fall) continue;
        if (D[i].ch[0] != txt[i]) {
            if (roll) { digit_roll(i, txt[i], delay); delay += 120; }
            else digit_char(&D[i], txt[i]);
        }
    }
}

static void digits_drop_in(float delay)
{
    for (int i = 0; i < ND; i++) { D[i].y = -D[i].h * (1 + i * .15f); D[i].vy = 0; D[i].fall = true; D[i].drop_at = s_now + delay + i * 70; }
}

static void digits_step(void)
{
    const float g2 = 0.22f * SPD * SPD;
    for (int i = 0; i < ND; i++) {
        digit_t *d = &D[i];
        if (d->fall) {
            if (s_now >= d->drop_at) {
                d->vy += g2 * 1.1f; d->y += d->vy;
                if (d->y >= d->hy) { d->y = d->hy; if (d->vy > 1.5f * SPD) { d->vy = -d->vy * .32f; d->sv += .3f; ripple(i, .8f); } else { d->vy = 0; d->fall = false; } }
            }
        } else {
            d->vx += (d->hx - d->x) * d->k * SPD * SPD - d->vx * d->c * SPD;
            d->vy += (d->hy - d->y) * d->k * SPD * SPD - d->vy * d->c * SPD;
            d->x += d->vx; d->y += d->vy;
        }
        float v = sqrtf(d->vx * d->vx + d->vy * d->vy) / SPD;
        float tg = 1 + fminf(.3f, fabsf(d->vx / SPD) * .06f) - fminf(.25f, fabsf(d->vy / SPD) * .07f);
        if (d->rip > 0 && s_now >= d->rip) { d->sv -= .32f * d->ripA; d->rip = 0; }
        if (!d->fall) { if (v < .3f && d->land) { d->sv += .22f; d->land = false; } if (v > 1.5f) d->land = true; }
        d->sv += (tg - d->sx) * .18f; d->sv *= .82f; d->sx += d->sv;
    }
#if DIGIT_GHOST_FADE
    if (G.on) { G.y += G.vy; G.rot += G.vr; G.a -= 1 / 16.f; if (G.a <= 0) { G.on = false; set_hidden(G.l, true); } }
#else
    if (G.on) { G.vy += g2; G.x += G.vx; G.y += G.vy; G.rot += G.vr; if (G.y > H + 150) { G.on = false; set_hidden(G.l, true); } }
#endif
}

static void digits_render(void)
{
    for (int i = 0; i < ND; i++) {
        digit_t *d = &D[i];
        float sx = clampf(d->sx, .5f, 1.6f), sy = 1 / sx;
        img_scale(d->l, (int32_t)lroundf(256 * sx), (int32_t)lroundf(256 * sy));
        set_opa(d->l, (d->fall && s_now < d->drop_at) ? 0 : 255);
        digit_place(d);
        // LVGL's own repaint box for a stretched picture can miss a sliver at its edge; repaint
        // where the digit is now and where it was, with a small margin, whenever it changes.
        float bl = d->f->base_line, pyv = d->y - d->yoff + d->h - bl;
        if (fabsf(sx - 1) < .02f) sx = sy = 1;
        lv_area_t b = { (int32_t)(d->x - d->w / 2.f * sx) - 6, (int32_t)(pyv - (d->h - bl) * sy) - 6,
                        (int32_t)(d->x + d->w / 2.f * sx) + 6, (int32_t)(pyv + bl * sy) + 6 };
        if (!d->box_ok || b.x1 != d->box.x1 || b.y1 != d->box.y1 || b.x2 != d->box.x2 || b.y2 != d->box.y2) {
            lv_obj_invalidate_area(s_page, &b);
            if (d->box_ok) lv_obj_invalidate_area(s_page, &d->box);
            d->box = b; d->box_ok = true;
        }
    }
    if (G.l) {
        lv_obj_set_pos(G.l, (int)(G.x - D[0].w / 2.f), (int)(G.y - (ND ? D[0].yoff : 0)));
        { int32_t r = (int32_t)(G.rot * 10) % 3600; if (r < 0) r += 3600; if ((int32_t)lv_image_get_rotation(G.l) != r) lv_image_set_rotation(G.l, r); }
        set_opa(G.l, (lv_opa_t)(255 * clampf(G.a, 0, 1)));
    }
}

// ================================================================== task rows
// Task text size is set from the phone (small .. extra large); rows grow with it.
static const lv_font_t *const TFONTS[4] = { &font_jp13, &font_jp16, &font_jp19, &font_jp22 };
static int s_tf = 0, s_rh = 34, s_rstep = 40;
#define TF (TFONTS[s_tf])
#define RH s_rh
#define RSTEP s_rstep
static bool tf_set(int lvl)
{
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    if (lvl == s_tf) return false;
    s_tf = lvl; s_rh = TF->line_height + 17; s_rstep = s_rh + 6;
    return true;
}
typedef struct { uint16_t id; lv_obj_t *o; lv_obj_t *halves[2]; float y, ty, a; bool done; char name[TASK_NAME_MAX]; float cut_t0; int cut_dir; bool cut_big, cut; } row_t;
static row_t R[MAX_TASKS]; static int NR;
static lv_obj_t *s_more;

static void row_fill(lv_obj_t *o, const char *name, bool done)
{
    lv_obj_clean(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(done ? 0x0E0E0E : 0x141414), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(done ? 0x222222 : 0x2E2E2E), 0);
    int cs = 12 + s_tf * 2, lx = 9 + cs + 8;                 // the box grows a little with the text
    lv_obj_t *ck = mk_rect(o, 9, RH / 2 - cs / 2 - 1, cs, cs, done ? C_MINT : 0x000000, 3);
    if (!done) { lv_obj_set_style_bg_opa(ck, 0, 0); lv_obj_set_style_border_width(ck, 2, 0); lv_obj_set_style_border_color(ck, lv_color_hex(0x8C8C8C), 0); }
    else icon_create(ck, IC_CHECK, cs / 2, cs / 2, cs, 0x06210F);   // the same exactly-drawn tick as everywhere else
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, name);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, 152 - lx - 5, TF->line_height);   // one line, "..." when too long
    lv_obj_set_style_text_font(l, TF, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(done ? 0x6E6E6E : 0xECEAE6), 0);
    if (done) lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    lv_obj_set_pos(l, lx, (RH - TF->line_height) / 2);
}

static lv_obj_t *row_obj(lv_obj_t *parent, const char *name, bool done)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 152, RH);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    row_fill(o, name, done);
    return o;
}

static float s_drag_dist;
static void row_clicked(lv_event_t *e)
{
    if (s_scr != SCR_TASKS || s_drag_dist > 8) return;   // that was a scroll, not a tap
    uint16_t id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    bool done = false;
    store_lock(); tasklist_t *tl = store_tasks(); for (int i = 0; i < tl->n; i++) if (tl->t[i].id == id) done = tl->t[i].done; store_unlock();
    store_task_set_done(id, !done);
    app_activity();
    if (!done) { ch_hop(2.4f); ch_burst_stars(); CH.expr = EX_HAPPY; }
}

// ---- Tasks list scrolling: drag (or fling) the list; rows leaving at the top roll back into the
// depth as if on a drum, and rows still to come roll up out of it at the bottom.
#define LIST_T 132.f
#define LIST_B 420.f
#define FOLD   40.f                    // height of each edge where rows curve away
static float s_scroll, s_scroll_v, s_drag_last;
static bool s_dragging;
static float scroll_max(void)
{
    int n = 0; for (int i = 0; i < NR; i++) if (!R[i].cut) n++;
    return fmaxf(0, n * RSTEP - (RSTEP - RH) - (LIST_B - LIST_T));
}
// Maps a row's logical top (already scrolled) to where it is drawn. Returns false when it has
// turned fully away. sy/sx squash it as it tilts, fo fades it.
static bool drum_map(float top, float *dtop, float *sx, float *sy, float *fo)
{
    float smax = scroll_max(), c = top + RH / 2;
    float ft = fminf(FOLD, s_scroll), fb = fminf(FOLD, smax - s_scroll);     // folds only where there is more
    float ct = LIST_T + RH / 2 + ft, cb = LIST_B - RH / 2 - fb;
    float d = c < ct ? ct - c : c > cb ? c - cb : 0, R = fmaxf(1, c < ct ? ft : fb);
    *sx = *sy = *fo = 1; *dtop = top;
    if (d <= 0) return c >= LIST_T && c <= LIST_B;
    float phi = d / R;                                                       // angle round the drum
    if (phi >= 1.45f) return false;
    float off = R * sinf(phi), k = cosf(phi);
    float dc = c < ct ? ct - off : cb + off;
    *sy = k; *sx = 1 - .22f * (1 - k); *fo = k * k;
    *dtop = dc - RH / 2;
    return true;
}
static void tasks_drag(lv_event_t *e)
{
    if (s_scr != SCR_TASKS) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    if (code == LV_EVENT_PRESSED) { s_dragging = true; s_drag_dist = 0; s_drag_last = p.y; s_scroll_v = 0; }
    else if (code == LV_EVENT_PRESSING && s_dragging) {
        float dy = p.y - s_drag_last; s_drag_last = p.y; s_drag_dist += fabsf(dy);
        if (s_drag_dist > 8) { s_scroll = clampf(s_scroll - dy, 0, scroll_max()); s_scroll_v = -dy; }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_dragging = false;
}
static void scroll_step(void)
{
    if (s_scr != SCR_TASKS || s_dragging) return;
    float smax = scroll_max();
    if (fabsf(s_scroll_v) > .4f) {
        s_scroll += s_scroll_v; s_scroll_v *= .93f;
        if (s_scroll < 0 || s_scroll > smax) { s_scroll = clampf(s_scroll, 0, smax); s_scroll_v = 0; }
    } else {
        s_scroll_v = 0;
        float snap = clampf(roundf(s_scroll / RSTEP) * RSTEP, 0, smax);     // settle on a whole row,
        if (fabsf(smax - s_scroll) < fabsf(snap - s_scroll)) snap = smax;     // or on the very end if that's nearer
        s_scroll += (snap - s_scroll) * .2f;
    }
}

static float rows_base(int n) { return s_scr == SCR_CLEAR ? 506 - n * RSTEP + (RSTEP - RH) : 132; }   // Clear: last row ends at 506, clear of the badge;
// a long list shows its bottom part there, where the checked tasks (the ones being cleared) are

// Sync rows with the task list. New rows fade in, removed rows disappear, targets follow the order.
static void rows_sync(bool initial)
{
    tasklist_t tl;
    store_lock(); tl = *store_tasks(); store_unlock();
    float base = rows_base(tl.n);
    float limit = s_scr == SCR_TASKS ? 430 : 506;
    // remove rows that no longer exist (unless they are mid-cut)
    for (int i = 0; i < NR; i++) {
        bool keep = false; for (int k = 0; k < tl.n; k++) if (tl.t[k].id == R[i].id) keep = true;
        if (!keep && !R[i].cut) { lv_obj_delete(R[i].o); R[i] = R[--NR]; i--; }
    }
    int hidden = 0;
    for (int k = 0; k < tl.n; k++) {
        row_t *r = NULL;
        for (int i = 0; i < NR; i++) if (R[i].id == tl.t[k].id) r = &R[i];
        float ty = base + k * RSTEP;
        if (!r) {
            r = &R[NR++]; memset(r, 0, sizeof *r);
            r->id = tl.t[k].id; r->done = tl.t[k].done; snprintf(r->name, sizeof r->name, "%s", tl.t[k].name);
            r->o = row_obj(s_page, r->name, r->done);
            r->y = ty; r->a = initial ? 1 : -.5f;
            lv_obj_add_flag(r->o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_set_style_transform_pivot_x(r->o, 76, 0); lv_obj_set_style_transform_pivot_y(r->o, RH / 2, 0);
            lv_obj_add_event_cb(r->o, row_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)r->id);
        } else if (r->done != tl.t[k].done || strcmp(r->name, tl.t[k].name)) {
            r->done = tl.t[k].done; snprintf(r->name, sizeof r->name, "%s", tl.t[k].name);
            row_fill(r->o, r->name, r->done);
        }
        r->ty = ty;
        if (s_scr != SCR_TASKS && ty + RH > limit) hidden++;
    }
    if (s_more && s_scr != SCR_TASKS) {   // "+3": the rest are on the phone page
        char b[12]; snprintf(b, sizeof b, "+%d", hidden);
        iitem_t it[1] = { { IC_NONE, b, 0x8C8C8C } };
        irow_set(&IR3, hidden ? 1 : 0, it);
    }
}

// The text size changed (from the phone): resize every row in place; rows_sync then moves them to
// their new places, so the list visibly grows or shrinks.
static void rows_restyle(void)
{
    for (int i = 0; i < NR; i++) {
        if (R[i].cut) continue;
        lv_obj_set_size(R[i].o, 152, RH);
        lv_obj_set_style_transform_pivot_y(R[i].o, RH / 2, 0);
        row_fill(R[i].o, R[i].name, R[i].done);
    }
    rows_sync(false);
    for (int i = 0; i < NR; i++) if (!R[i].cut && R[i].a < 1) R[i].a = 1;   // already on screen: no fade
}

static void rows_step(void)
{
    float limit = s_scr == SCR_TASKS ? 430 : 506;
    scroll_step();
    if (s_more && s_scr == SCR_TASKS) {    // "+3": that many more below; scroll to see them
        float smax = scroll_max(); int below = (int)ceilf((smax - s_scroll - 2) / RSTEP);
        char b[12]; snprintf(b, sizeof b, "+%d", below);
        iitem_t it[1] = { { IC_NONE, b, 0x8C8C8C } };
        irow_set(&IR3, below > 0 ? 1 : 0, it);
    }
    // Rows never slide across each other or into a slot that is still being cut:
    // on Tasks a row that changes place fades out, jumps, and fades back in; on Clear the rest
    // wait until the cut halves are gone, then close up (their order never changes there).
    bool cutting = false;
    for (int i = 0; i < NR; i++) if (R[i].cut && s_now - R[i].cut_t0 < 820) cutting = true;
    bool fading = false;
    for (int i = 0; i < NR; i++) if (!R[i].cut && fabsf(R[i].ty - R[i].y) > .5f && R[i].a > 0) fading = true;
    for (int i = 0; i < NR; i++) {
        row_t *r = &R[i];
        if (s_scr == SCR_TASKS && !r->cut) {
            if (fabsf(r->ty - r->y) > .5f) { r->a -= .12f; if (r->a <= 0) { r->y = r->ty; r->a = -.2f; } }
            else if (!fading) r->a = fminf(1, r->a + .08f);
        } else {
            if (!cutting || r->cut) r->y += (r->ty - r->y) * .15f;
            r->a = fminf(1, r->a + .05f);
        }
        if (r->cut) {
            float e = s_now - r->cut_t0;
            if (e < 0) { lv_obj_set_pos(r->o, 10, (int)r->y); continue; }
            if (e < 260) {
                lv_obj_set_pos(r->o, 10, (int)r->y);
                if (e > 140) lv_obj_set_style_bg_color(r->o, lv_color_hex(0xFFFFFF), 0);
                continue;
            }
            if (!r->halves[0]) {
                // split: two clipped copies fly apart
                for (int h = 0; h < 2; h++) {
                    lv_obj_t *clip = lv_obj_create(s_page);
                    lv_obj_remove_style_all(clip);
                    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_size(clip, 152, RH / 2);
                    lv_obj_t *copy = row_obj(clip, r->name, r->done);
                    lv_obj_set_pos(copy, 0, h ? -RH / 2 : 0);
                    lv_obj_set_style_transform_pivot_x(clip, 76, 0);
                    lv_obj_set_style_transform_pivot_y(clip, RH / 4, 0);
                    r->halves[h] = clip;
                }
                lv_obj_add_flag(r->o, LV_OBJ_FLAG_HIDDEN);
            }
            float p = easeOut(clampf((e - 260) / 540, 0, 1)), k = r->cut_big ? 1.8f : 1;
            for (int h = 0; h < 2; h++) {
                float sgn = h ? 1 : -1;
                lv_obj_set_pos(r->halves[h], (int)(10 + sgn * 70 * p * k), (int)(r->y + (h ? RH / 2 : 0)));   // sideways, inside its own slot
                set_opa(r->halves[h], (lv_opa_t)(255 * (1 - p)));
            }
            continue;
        }
        float dtop = r->y, sx = 1, sy = 1, fo = 1;
        bool vis = s_scr == SCR_TASKS ? drum_map(r->y - s_scroll, &dtop, &sx, &sy, &fo) : r->ty + RH <= limit && r->ty >= 44;
        set_hidden(r->o, !vis);
        lv_obj_set_pos(r->o, 10, (int)lroundf(dtop));
        if (s_scr == SCR_TASKS) {
            set_scale(r->o, (int32_t)lroundf(256 * sx), (int32_t)lroundf(256 * sy));
        }
        set_opa(r->o, (lv_opa_t)(255 * clampf(r->a, 0, 1) * fo));
    }
}

static void rows_clear_all(void)
{
    for (int i = 0; i < NR; i++) R[i].o = NULL;
    NR = 0; s_more = NULL;
}

// ================================================================== per-screen state
static struct {
    lv_obj_t *l1, *l2, *l3, *bar, *qrbox, *qr, *dim, *tier;
    int sec, minute; float next_ev; int ev; float ev_t0; float pts[3][2]; int pi; float wait; float look_x;
    int net_shown; char qr_txt[96];
    int tier_n, tier_kind; float tier_t; int phase; float t1, end; int cutk; float dimv; bool dim_fit;
    bool woke; float wake_t; float hop_t; int last_page; float press_t;
} M;

static struct { bool on; screen_t s; orient_t o; float turn; bool roll; char old[3]; char val[4]; } P;

// True while the timer digits are still arriving (screen change pending, digits falling or bouncing in).
// The countdown waits for this, so the first number you see is the full time and none are skipped.
bool ui_digits_busy(void)
{
    if (P.on) return true;
    for (int i = 0; i < ND; i++) if (D[i].fall) return true;
    return false;
}


static void ov_invalidate(void);

// ------------------------------------------------------------------ build
// LVGL draws the overlay once per repainted area and allocates a draw task for every shape, even
// ones outside the area. Shapes that can't touch the area being drawn are skipped up front.
static inline bool clip_hit(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const lv_area_t *c = &layer->_clip_area;
    return x2 >= c->x1 && x1 <= c->x2 && y2 >= c->y1 && y1 <= c->y2;
}
static lv_area_t s_ch_box; static bool s_ch_box_ok;   // the character's exact box this frame, when measured

// ------------------------------------------------------------------ rain (Home screen)
#define NDROP 48
#define NSPL 24
#define RAIN_FLOOR 140.f
static struct {
    bool on; float amt;                 // amt eases toward the target intensity (0 = off)
    float target; int check;            // frames until the forecast is read again
    rain_info_t info;
    bool snow;                          // flakes instead of drops
    struct { float x, y, v, len, ph; } d[NDROP];
    struct { float x, y, vx, vy, life; } s[NSPL];
    float look_t;
} RN;

// Drops and flakes are never drawn within KEEP px of the clock, the bottom row, the battery or the eyes
// (they pass behind them). They still land on the umbrella: that is the point of it.
#define KEEP 7
static lv_area_t s_keep[24]; static int s_nkeep;
static void keep_add(float x0, float y0, float x1, float y1)
{
    if (s_nkeep < 24) s_keep[s_nkeep++] = (lv_area_t){ (int32_t)x0 - KEEP, (int32_t)y0 - KEEP, (int32_t)x1 + KEEP, (int32_t)y1 + KEEP };
}
static void keep_obj(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(o, &a); keep_add(a.x1, a.y1, a.x2, a.y2);
}
static bool kept_out(float x0, float y0, float x1, float y1)
{
    for (int i = 0; i < s_nkeep; i++) if (x1 >= s_keep[i].x1 && x0 <= s_keep[i].x2 && y1 >= s_keep[i].y1 && y0 <= s_keep[i].y2) return true;
    return false;
}
static void rain_drop_reset(int i, bool anywhere)
{
    RN.d[i].x = frand() * (W + 40);
    RN.d[i].y = anywhere ? frand() * RAIN_FLOOR : -frand() * 60 - 10;
    RN.d[i].ph = frand() * 6.28f;
    if (RN.snow) { RN.d[i].v = .55f + frand() * .8f; RN.d[i].len = 1.2f + frand() * 1.4f; }   // len = flake radius
    else { RN.d[i].v = 6.5f + frand() * 2.5f; RN.d[i].len = 8 + frand() * 6; }
}
static void rain_splash(float x, float y, int n)
{
    for (int k = 0; k < n; k++) for (int j = 0; j < NSPL; j++) if (RN.s[j].life <= 0) {
        RN.s[j].x = x; RN.s[j].y = y; RN.s[j].vx = (frand() - .5f) * 3.2f; RN.s[j].vy = -1 - frand() * 1.6f; RN.s[j].life = 1; break;
    }
}
static void rain_read(void)
{
    rain_info_t ri;
    bool have = weather_rain_today(&ri);
    RN.info = have ? ri : (rain_info_t){ 0 };
    RN.target = RN.info.rain ? RN.info.intensity : 0;
    if (RN.info.rain && RN.info.snow != RN.snow) {          // switched between rain and snow
        RN.snow = RN.info.snow;
        for (int i = 0; i < NDROP; i++) rain_drop_reset(i, true);
    }
}
static void rain_init(void)
{
    memset(&RN, 0, sizeof RN);
    rain_read();
    RN.amt = RN.target;                                   // already raining when Home appears
    RN.snow = RN.info.snow;
    for (int i = 0; i < NDROP; i++) rain_drop_reset(i, true);
    RN.check = 60;
}
static void rain_step(void)
{
    if (--RN.check <= 0) { rain_read(); RN.check = 60; }
    RN.amt += (RN.target - RN.amt) * .02f;
    int active = RN.amt < .02f ? 0 : (int)(10 + RN.amt * (NDROP - 10));
    for (int i = 0; i < NDROP; i++) {
        float vx = RN.snow ? sinf(s_now / 900 + RN.d[i].ph) * .45f - .1f : -.22f * RN.d[i].v;
        RN.d[i].x += vx; RN.d[i].y += RN.d[i].v;
        if (RN.d[i].x < -10) RN.d[i].x += W + 30;
        if (i >= active) { if (RN.d[i].y > RAIN_FLOOR) rain_drop_reset(i, false); continue; }
        if (ch_umbrella_hit(RN.d[i].x, RN.d[i].y)) {
            if (RN.snow) CH.snow_cap = fminf(1, CH.snow_cap + .012f);   // snow piles up on the canopy
            else rain_splash(RN.d[i].x, RN.d[i].y, 2);
            rain_drop_reset(i, false); continue;
        }
        if (RN.snow && RN.d[i].y > RAIN_FLOOR) { rain_drop_reset(i, false); continue; }
        if (RN.d[i].y > RAIN_FLOOR) { if (frand() < .5f) rain_splash(RN.d[i].x, RAIN_FLOOR, 1); rain_drop_reset(i, false); }
    }
    if (!RN.snow || !RN.info.rain) CH.snow_cap = fmaxf(0, CH.snow_cap - .002f);   // melts when it stops
    // water running off the rim now and then
    if (!RN.snow && CH.umb_a > .9f && RN.amt > .05f && frand() < .06f + RN.amt * .1f) {
        float x, y; ch_umbrella_edge(frand() < .5f ? -1 : 1, &x, &y);
        for (int j = 0; j < NSPL; j++) if (RN.s[j].life <= 0) { RN.s[j].x = x; RN.s[j].y = y + 2; RN.s[j].vx = 0; RN.s[j].vy = .5f; RN.s[j].life = 1.6f; break; }
    }
    for (int j = 0; j < NSPL; j++) if (RN.s[j].life > 0) {
        RN.s[j].x += RN.s[j].vx; RN.s[j].y += RN.s[j].vy; RN.s[j].vy += .22f; RN.s[j].life -= 1 / 16.f;
        if (RN.s[j].y > RAIN_FLOOR + 1) RN.s[j].life = 0;
    }
}
// A raindrop is a short slanted line. Drawing it as a line costs a slanted-edge mask per pixel,
// every frame, for every drop, so each shape (length x slant) is drawn once into a small
// coverage picture and then just stamped in the drop colour. It looks exactly the same.
static lv_draw_buf_t *s_drop[18][6];
static const lv_image_dsc_t *drop_img(int len, int dx, bool make)   // make: only outside drawing
{
    if (len < 1 || len > 17 || dx < 0 || dx > 5) return NULL;
    if (s_drop[len][dx] || !make) return (const lv_image_dsc_t *)s_drop[len][dx];
    int w = dx + 5, h = len + 5;
    lv_draw_buf_t *b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!b) return NULL;
    lv_draw_buf_clear(b, NULL);
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, b);
    lv_layer_t ly; lv_canvas_init_layer(cv, &ly);
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l);
    l.color = lv_color_white(); l.width = 2; l.round_start = l.round_end = 1;
    l.p1.x = 2 + dx; l.p1.y = 2; l.p2.x = 2; l.p2.y = 2 + len;
    lv_draw_line(&ly, &l);
    lv_canvas_finish_layer(cv, &ly);
    lv_obj_delete(cv);
    lv_draw_buf_t *m = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    if (!m) { lv_draw_buf_destroy(b); return NULL; }
    for (int y = 0; y < h; y++) {
        const uint8_t *src = b->data + y * b->header.stride; uint8_t *dst = m->data + y * m->header.stride;
        for (int x = 0; x < w; x++) dst[x] = src[x * 4 + 3];
    }
    lv_draw_buf_destroy(b);
    s_drop[len][dx] = m;
    return (const lv_image_dsc_t *)m;
}
static uint32_t s_frame_no, s_keep_frame = UINT32_MAX;
static void rain_draw(lv_layer_t *layer)
{
    if (!clip_hit(layer, -10, -20, W + 10, (int32_t)RAIN_FLOOR + 6)) return;
    // what the rain must stay clear of, measured now (digits may be mid-fall); once per frame,
    // not once per repainted area
    if (s_keep_frame != s_frame_no) { s_keep_frame = s_frame_no;
    s_nkeep = 0;
    for (int i = 0; i < ND; i++) {                                      // digits as drawn: squash and stretch included
        lv_area_t a; lv_obj_get_coords(D[i].l, &a);
        float sx = clampf(D[i].sx, .5f, 1.6f), sy = 1 / sx, cx = (a.x1 + a.x2) / 2.f, w = a.x2 - a.x1, h = a.y2 - a.y1;
        keep_add(cx - w * sx / 2, a.y2 - h * sy, cx + w * sx / 2, a.y2);
    }
    if (G.on) { lv_area_t a; lv_obj_get_coords(G.l, &a); float w = a.x2 - a.x1; keep_add(a.x1 - w / 3, a.y1 - w / 3, a.x2 + w / 3, a.y2 + w / 3); }
    for (int i = 0; i < IR_MAX; i++) { keep_obj(IR1.ic[i]); keep_obj(IR1.tx[i]); keep_obj(IR2.ic[i]); keep_obj(IR2.tx[i]); }
    keep_add(W - 30, 7, W - 10, 16);                                   // battery
    { float x0, y0, x1, y1; ch_eye_box(&x0, &y0, &x1, &y1); keep_add(x0, y0, x1, y1); } }
    int active = RN.amt < .02f ? 0 : (int)(10 + RN.amt * (NDROP - 10));
    if (RN.snow) {
        lv_draw_rect_dsc_t f; lv_draw_rect_dsc_init(&f); f.bg_color = lv_color_white(); f.radius = LV_RADIUS_CIRCLE;
        for (int i = 0; i < active; i++) {
            if (RN.d[i].y < -4) continue;
            float r = RN.d[i].len;
            f.bg_opa = (lv_opa_t)(120 + 50 * r);
            lv_area_t a = { lroundf(RN.d[i].x - r), lroundf(RN.d[i].y - r), lroundf(RN.d[i].x + r), lroundf(RN.d[i].y + r) };
            if (!clip_hit(layer, a.x1, a.y1, a.x2, a.y2) || kept_out(a.x1, a.y1, a.x2, a.y2)) continue;
            lv_draw_rect(layer, &f, &a);
        }
        return;
    }
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l);
    l.color = lv_color_hex(0x7FB2FF); l.width = 2; l.round_start = l.round_end = 1;
    lv_draw_image_dsc_t im; lv_draw_image_dsc_init(&im);
    im.recolor = l.color; im.recolor_opa = LV_OPA_COVER;
    for (int i = 0; i < active; i++) {
        if (RN.d[i].y < 0) continue;
        int32_t ax = lroundf(RN.d[i].x + .22f * RN.d[i].len), ay = lroundf(RN.d[i].y - RN.d[i].len), bx = lroundf(RN.d[i].x), by = lroundf(RN.d[i].y);
        if (ax == bx && ay == by) continue;               // LVGL 9.2 zero-length line bug
        if (!clip_hit(layer, LV_MIN(ax, bx) - 2, ay - 2, LV_MAX(ax, bx) + 2, by + 2)) continue;
        if (kept_out(fminf(ax, bx) - 1, ay - 1, fmaxf(ax, bx) + 1, by + 1)) continue;
        lv_opa_t op = (lv_opa_t)(70 + 60 * fminf(1, RN.amt * 1.5f));
        const lv_image_dsc_t *img = drop_img(by - ay, ax - bx, false);
        if (img) {
            im.src = img; im.opa = op;
            lv_area_t ia = { bx - 2, ay - 2, bx - 2 + (int32_t)img->header.w - 1, ay - 2 + (int32_t)img->header.h - 1 };
            lv_draw_image(layer, &im, &ia);
        } else { l.p1.x = ax; l.p1.y = ay; l.p2.x = bx; l.p2.y = by; l.opa = op; lv_draw_line(layer, &l); }
    }
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.bg_color = lv_color_hex(0xA9CBFF); r.radius = LV_RADIUS_CIRCLE;
    for (int j = 0; j < NSPL; j++) if (RN.s[j].life > 0) {
        r.bg_opa = (lv_opa_t)(200 * fminf(1, RN.s[j].life));
        lv_area_t a = { lroundf(RN.s[j].x) - 1, lroundf(RN.s[j].y) - 1, lroundf(RN.s[j].x) + 1, lroundf(RN.s[j].y) + 1 };
        if (!clip_hit(layer, a.x1, a.y1, a.x2, a.y2) || kept_out(a.x1, a.y1, a.x2, a.y2)) continue;
        lv_draw_rect(layer, &r, &a);
    }
}

// The boxes the rain covers right now, one per drop, flake and splash (a bit generous).
#define RAIN_RAW 80
static int rain_boxes(lv_area_t *o)
{
    int n = 0, active = RN.amt < .02f ? 0 : (int)(10 + RN.amt * (NDROP - 10));
    for (int i = 0; i < active && n < RAIN_RAW; i++) {
        float x = RN.d[i].x, y = RN.d[i].y, l = RN.d[i].len;
        if (RN.snow) { if (y < -4) continue; o[n++] = (lv_area_t){ (int32_t)(x - l) - 2, (int32_t)(y - l) - 2, (int32_t)(x + l) + 2, (int32_t)(y + l) + 2 }; }
        else { if (y < 0) continue; o[n++] = (lv_area_t){ (int32_t)x - 3, (int32_t)(y - l) - 3, (int32_t)(x + .22f * l) + 3, (int32_t)y + 3 }; }
    }
    for (int j = 0; j < NSPL && n < RAIN_RAW; j++) if (RN.s[j].life > 0)
        o[n++] = (lv_area_t){ (int32_t)RN.s[j].x - 3, (int32_t)RN.s[j].y - 3, (int32_t)RN.s[j].x + 3, (int32_t)RN.s[j].y + 3 };
    return n;
}
static int32_t a_size(const lv_area_t *a) { return (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1); }
// Merges boxes (sorted left to right) until at most `max` remain, each time joining the two
// neighbours whose union wastes the least. Keeps the repaint to roughly the drops themselves.
static int boxes_merge(lv_area_t *b, int n, int max)
{
    for (int i = 1; i < n; i++) { lv_area_t k = b[i]; int j = i - 1; while (j >= 0 && b[j].x1 > k.x1) { b[j + 1] = b[j]; j--; } b[j + 1] = k; }
    while (n > max) {
        int best = 0; int32_t bc = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t u = { LV_MIN(b[i].x1, b[i + 1].x1), LV_MIN(b[i].y1, b[i + 1].y1), LV_MAX(b[i].x2, b[i + 1].x2), LV_MAX(b[i].y2, b[i + 1].y2) };
            int32_t c = a_size(&u) - a_size(&b[i]) - a_size(&b[i + 1]);
            if (c < bc) { bc = c; best = i; }
        }
        lv_area_t *p = &b[best], *q = &b[best + 1];
        *p = (lv_area_t){ LV_MIN(p->x1, q->x1), LV_MIN(p->y1, q->y1), LV_MAX(p->x2, q->x2), LV_MAX(p->y2, q->y2) };
        memmove(q, q + 1, sizeof *q * (n - best - 2)); n--;
    }
    return n;
}

static void build_home(void)
{
    bool low = app_low_battery();
    ch_reset(W, H, 92, 72, .85f);
    CH.ly1 = 134;                                        // stays above the bottom row
    rain_init();
    // the umbrella opens (rises into place) once the character has spun upright after a turn
    CH.umbrella = RN.info.rain; CH.umb_a = 0;
    if (RN.info.rain && RN.info.snow && RN.info.now) CH.snow_cap = .5f;
    if (CH.umbrella) CH.home_y = CH.y = CH.ty = 80;    // a little lower, so hops keep the umbrella on screen
    // with the umbrella up there's less headroom, so the stroll stays lower
    if (low) ch_free_box(90, 120, 66, 82); else if (CH.umbrella) ch_free_box(80, 160, 74, 92); else ch_free_box(70, 170, 46, 100);
    CH.idle = true;
    if (low) { CH.breath_amp = 3; CH.breath_ms = 1400; }
    char hm[6] = "--:--";
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    if (rtc_time_valid()) snprintf(hm, sizeof hm, "%02d:%02d", lt.tm_hour, lt.tm_min);
    M.minute = rtc_time_valid() ? lt.tm_min : -1;       // -1: roll in the real time once it's set
    make_digits(hm, &font_num132, 404, 84, 0xFFFFFF);  // centred, and clear of the bottom row (never overlaps)
    char date[48];
    if (rtc_time_valid()) strftime(date, sizeof date, "%a %m/%d", &lt); else date[0] = 0;
    (void)date; M.l1 = NULL;                             // no date line: the clock is enough
    irow_make(&IR1, 24, 165, 0, 26, &font_num28);        // [tick] sessions done today
    irow_make(&IR2, 616, 165, 2, 26, &font_num28);       // only when it matters: paused / battery / rain
    M.next_ev = s_now + 6000;
}

static void tick_home(void)
{
    char b[64];
    store_lock();
    session_t ss = *store_session(); tasklist_t *tl = store_tasks();
    int done = 0; for (int i = 0; i < tl->n; i++) done += tl->t[i].done;
    int n = tl->n;
    store_unlock();
    char ns[8], nt[12]; snprintf(ns, sizeof ns, "%u", ss.sessions); snprintf(nt, sizeof nt, "%d/%d", done, n);
    (void)nt;
    iitem_t left[1] = { { IC_DONE, ns, C_AMBER } };
    irow_set(&IR1, 1, left);
    bool low = app_low_battery();
    if (ss.study_left_ms > 0 && !ss.study_running) {               // paused study: [pause] 12:34  [flip]
        int sec = (ss.study_left_ms + 999) / 1000;
        snprintf(b, sizeof b, "%02d:%02d", sec / 60, sec % 60);
        iitem_t it[1] = { { IC_PAUSE, b, C_AMBER } };
        irow_set(&IR2, 1, it);
    } else if (RN.info.rain) {                                      // [umbrella] 15:00 40%   or   [snow] 80%
        if (RN.info.now) snprintf(b, sizeof b, "%d%%", RN.info.max_prob);
        else snprintf(b, sizeof b, "%02d:00 %d%%", RN.info.from_hour, RN.info.max_prob);
        iitem_t it[1] = { { RN.info.snow ? IC_SNOW : IC_UMBRELLA, b, RN.info.snow ? 0xE6F0FF : C_SKY } };
        irow_set(&IR2, 1, it);
    } else irow_set(&IR2, 0, NULL);                                 // nothing to say

    // clock: roll the digits that changed
    if (rtc_time_valid()) {
        time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
        if (lt.tm_min != M.minute) {
            M.minute = lt.tm_min;
            char hm[6]; snprintf(hm, sizeof hm, "%02d:%02d", lt.tm_hour, lt.tm_min);
            digits_set(hm, true);
            if (!M.ev) { M.ev = 2; M.ev_t0 = s_now; CH.idle = false; }
            char date[48]; strftime(date, sizeof date, "%a %m/%d", &lt); relabel(M.l1, date);
        }
    }
    // rain: umbrella goes up or down as the forecast changes
    rain_step();
    if (CH.umbrella != RN.info.rain) {
        CH.umbrella = RN.info.rain;
        CH.home_y = CH.umbrella ? 80 : 72;
        if (CH.umbrella) ch_free_box(80, 160, 74, 92); else if (!low) ch_free_box(70, 170, 46, 100);
        if (!M.ev && !CH.grav) ch_seek(CH.home_x, CH.home_y, 0, 0);
        if (!M.ev) { CH.expr = EX_SURPRISED; ch_hop(2.2f); }
    }
    // character
    float t = s_now;
    if (M.woke && t - M.wake_t < 900) { CH.expr = t - M.wake_t < 450 ? EX_SURPRISED : EX_HAPPY; }
    if (!M.ev) {
        CH.idle = true;
        if (low) { float y = fmodf(t, 6200); CH.expr = (y > 4300 && y < 5000) ? EX_SLEEP : EX_SAD; CH.tilt_ov_on = true; CH.tilt_ov = 8 + sinf(t / 1300) * 3; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = 3; }
        else if (!(M.woke && t - M.wake_t < 900)) {
            CH.expr = EX_NORMAL; CH.tilt_ov_on = false; CH.gaze_ov_on = false;
            // under the umbrella: now and then peek up at the rain
            if (CH.umbrella && RN.amt > .05f) {
                if (t > RN.look_t + 9000) RN.look_t = t + frand() * 3000;
                float d = t - RN.look_t;
                if (d > 0 && d < 1100) { CH.gaze_ov_on = true; CH.gaze_x = d < 550 ? -6 : 6; CH.gaze_y = -9; }
            }
        }
        if (!low && t > M.next_ev && CH.fx1 - CH.fx0 > 60) {
            M.ev = 1; M.ev_t0 = t; CH.idle = false; M.pi = 0; M.wait = -1;
            for (int i = 0; i < 2; i++) { M.pts[i][0] = CH.fx0 + frand() * (CH.fx1 - CH.fx0); M.pts[i][1] = CH.fy0 + frand() * (CH.fy1 - CH.fy0); }
            M.pts[2][0] = CH.home_x; M.pts[2][1] = CH.home_y;
        }
    } else if (M.ev == 1) {          // calm stroll
        float *p = M.pts[M.pi];
        if (M.wait < 0) {
            CH.expr = EX_NORMAL; CH.gaze_ov_on = false;
            ch_seek(p[0], p[1], .007f, 1.8f);
            if (fabsf(CH.x - p[0]) + fabsf(CH.y - p[1]) < 12 && fabsf(CH.vx) + fabsf(CH.vy) < .5f) { M.wait = t; M.look_x = (frand() < .5f ? -1 : 1) * 10; }
        } else {
            float w = t - M.wait;
            CH.gaze_ov_on = true; CH.gaze_x = w < 500 ? M.look_x : -M.look_x * .6f; CH.gaze_y = w < 500 ? -3 : -1;
            if (w > 1000) { M.pi++; M.wait = -1; CH.gaze_ov_on = false; if (M.pi >= 3) { M.ev = 0; M.next_ev = t + 9000 + frand() * 6000; } }
        }
    } else if (M.ev == 2) {          // the minute just rolled over
        float d = t - M.ev_t0;
        CH.gaze_ov_on = true; CH.gaze_x = 13; CH.gaze_y = -6;
        if (d < 700) CH.expr = EX_SURPRISED;
        else if (d < 720 && !low) ch_hop(2.8f);
        else { CH.expr = low ? EX_SAD : EX_HAPPY; CH.gaze_x = 8; CH.gaze_y = low ? 1 : -2; }
        if (d > 2000) { M.ev = 0; CH.gaze_ov_on = false; CH.grav = false; M.next_ev = t + 6000 + frand() * 4000; }
    }
}

// A countdown bar: the coloured part is the time still left. It is pinned to the right end and
// shrinks from the left until nothing is left.
static void bar_left(lv_obj_t *bar, float left)
{
    int w = (int)lroundf(608 * clampf(left, 0, 1));
    if (w < 2) { set_hidden(bar, true); return; }
    set_hidden(bar, false);
    lv_obj_set_width(bar, w);
    lv_obj_set_x(bar, 16 + 608 - w);
}

static void build_study(void)
{
    ch_reset(W, H, 565, 74, .72f);                       // a bit smaller and further right: the timer gets the room
    CH.ly1 = 152; CH.lx0 = 440;                          // above the bar, right of the timer
    CH.book = true; CH.think_left = true;
    session_t ss; store_lock(); ss = *store_session(); store_unlock();
    int s = ss.study_left_ms < 0 ? store_settings()->study_min * 60 : (ss.study_left_ms + 999) / 1000;
    char b[8]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    M.sec = s;
    make_digits(b, &font_num150, 218, 78, 0xFFFFFF);
    M.l1 = icon_create(s_page, IC_NONE, 466, 78, 48, C_AMBER);         // only when paused / ready (no bubble then)
    M.l2 = NULL; M.l3 = NULL;
    M.ev = 0; M.ev_t0 = -1e9f;
    M.next_ev = s_now + 20000 + frand() * 20000;
    mk_rect(s_page, 16, 160, 608, 6, 0x262626, 3);
    M.bar = mk_rect(s_page, 16, 160, 608, 6, C_AMBER, 3);
}

static void tick_study(void)
{
    session_t ss; settings_t st;
    store_lock(); ss = *store_session(); st = *store_settings(); store_unlock();
    char b[48];
    bool idle = !ss.study_running && !ss.carry;            // paused, or ready after "Session ended"
    bool fresh = ss.study_left_ms < 0;
    if (idle) { icon_set(M.l1, fresh ? IC_PLAY : IC_PAUSE); icon_set_color(M.l1, fresh ? 0xFFFFFF : C_AMBER); }
    else icon_set(M.l1, IC_NONE);
    int s = ss.study_left_ms < 0 ? (idle ? st.study_min * 60 : 0) : (ss.study_left_ms + 999) / 1000;
    if (s != M.sec) { M.sec = s; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); digits_set(b, false); }
    float prog = ss.study_left_ms < 0 ? (idle ? 0 : 1) : 1 - ss.study_left_ms / (st.study_min * 60000.f);
    bar_left(M.bar, 1 - prog);                               // time left: starts full, runs down to nothing

    CH.book = !idle;
    if (s_now - M.press_t < 900) { CH.expr = s_now - M.press_t < 450 ? EX_SURPRISED : (ss.study_running ? EX_DETERMINED : EX_NORMAL); CH.think_on = false; CH.gaze_ov_on = false; return; }
    if (idle) {                                            // book down, looking at you, slow blink rhythm
        M.next_ev = s_now + 20000 + frand() * 20000;       // no thought right after resuming
        CH.think_on = false; CH.tilt_ov_on = false; CH.page_flip = -1;
        CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = 0;
        CH.expr = EX_NORMAL;
        return;
    }
    // a thought now and then, at random 20-40 s apart, so it never becomes a beat to wait for
    if (s_now >= M.next_ev) { M.ev_t0 = s_now; M.ev++; M.next_ev = s_now + 20000 + frand() * 20000; }
    float d = s_now - s_t0, e = s_now - M.ev_t0;
    bool thinking = e >= 0 && e < 2800;
    if (thinking) {
        CH.page_flip = -1; CH.gaze_ov_on = true; CH.gaze_x = -11; CH.gaze_y = -10; CH.tilt_ov_on = true; CH.tilt_ov = 8;
        CH.think_on = true; CH.think_e = e; CH.think_bang = (M.ev % 2 == 1) && e > 2000;   // every other thought: an idea
        CH.expr = CH.think_bang ? EX_HAPPY : EX_NORMAL;
        return;
    }
    CH.think_on = false; CH.tilt_ov_on = false;
    int line = (int)(d / 2400); float ld = fmodf(d, 2400);
    bool glance = line % 3 == 2 && ld < 1000;
    float ly = 8 + (line % 4) * 1.6f;
    CH.page_flip = (line > 0 && line % 4 == 0 && ld < 450) ? ld / 450 : -1;
    CH.gaze_ov_on = true;
    if (glance) { CH.gaze_x = 0; CH.gaze_y = -3; CH.expr = EX_NORMAL; }
    else {
        const float SW = 1950;
        if (ld < SW) { CH.gaze_x = -14 + 28 * (ld / SW); CH.gaze_y = ly; }
        else { float p = clampf((ld - SW) / 450, 0, 1); p = p < .5f ? 4 * p * p * p : 1 - powf(-2 * p + 2, 3) / 2; CH.gaze_x = 14 - 28 * p; CH.gaze_y = ly + 1.6f * p; }
        CH.expr = EX_FOCUS;
    }
}

static void build_done(void)
{
    ch_reset(W, H, 530, 96, .9f);
    CH.lx0 = 300;                                        // right of the count
    CH.grav = true; CH.floor_y = 96;
    icon_create(s_page, IC_DONE_FLIP, 92, 86, 84, C_MINT);                   // session finished
    M.l1 = mk_label(s_page, "1", &font_num92, 0xFFFFFF, 160, 120, 0);   // today's count
    M.ev = 0; M.sec = -1;
}

static void tick_done(void)
{
    session_t ss; store_lock(); ss = *store_session(); store_unlock();
    char b[64];
    snprintf(b, sizeof b, "%u", ss.sessions); relabel(M.l1, b);
    float d = s_now - s_t0, c = fmodf(d, 5800); int cy = (int)(d / 5800);
    if (cy != M.sec) { M.sec = cy; M.ev = 0; }
    if (ss.skip_break) { CH.expr = EX_NORMAL; CH.tilt_ov_on = true; CH.tilt_ov = sinf(d / 300) * 6; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = -2; return; }
    const float js[3] = { 200, 950, 1700 };
    if (M.ev < 3 && c > js[M.ev]) { CH.grav = true; CH.floor_y = CH.home_y; CH.vy = -4.2f * SPD; M.ev++; ch_burst_stars(); CH.expr = EX_HAPPY; CH.tilt_ov_on = false; }
    if (c > 2600 && c < 3400) { CH.expr = EX_SURPRISED; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = -3; }
    if (c >= 3400) { CH.expr = EX_DETERMINED; CH.gaze_ov_on = false; CH.tilt_ov_on = true; CH.tilt_ov = sinf((c - 3400) / 160) * 20; }
    else if (c < 2600) CH.tilt_ov_on = false;
}

static void build_break(void)
{
    ch_reset(W, H, 104, 92, 1.05f);                      // smaller sleeper on the left: the timer gets the room
    CH.ly1 = 152; CH.lx1 = 196;                          // above the bar, left of the timer
    CH.zzz = true; CH.breath_amp = 4; CH.breath_ms = 1100;
    session_t ss; store_lock(); ss = *store_session(); store_unlock();
    int s = (ss.break_left_ms + 999) / 1000; M.sec = s;
    char b[8]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    make_digits(b, &font_num150, 408, 86, 0xFFFFFF);
    mk_rect(s_page, 16, 160, 608, 6, 0x1F1F1F, 3);
    M.bar = mk_rect(s_page, 16, 160, 608, 6, C_SKY, 3);
}

static void tick_break(void)
{
    session_t ss; settings_t st; store_lock(); ss = *store_session(); st = *store_settings(); store_unlock();
    int s = (ss.break_left_ms + 999) / 1000;
    if (s != M.sec) { M.sec = s; char b[8]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); digits_set(b, false); }
    float total = ss.break_total_ms > 0 ? ss.break_total_ms : st.break_min * 60000.f;
    bar_left(M.bar, ss.break_left_ms / total);
    float d = s_now - s_t0;
    if (ss.break_left_ms < 2500) { CH.expr = EX_SURPRISED; CH.zzz = false; CH.tilt_ov_on = true; CH.tilt_ov = 0; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = -3; return; }
    float c = fmodf(d, 9000); bool peek = c > 5200 && c < 6600;
    CH.expr = peek ? EX_PEEK : EX_SLEEP; CH.zzz = !peek; CH.tilt_ov_on = true; CH.tilt_ov = peek ? -6 : 9 + sinf(d / 1100) * 3;
    CH.gaze_ov_on = true; CH.gaze_x = peek ? 6 : 0; CH.gaze_y = peek ? -2 : 4;
}

static void build_tasks(void)
{
    ch_reset(W, H, 86, 56, .5f);
    CH.ly1 = 88;                                         // above the count
    M.l1 = mk_label(s_page, "0/0", &font_num28, C_AMBER, 86, 120, 1);   // done / total
    irow_make(&IR3, 86, 452, 1, 0, &font_num28);                        // +3: the rest are on the phone
    s_more = M.l1;
    mk_rect(s_page, 10, 458, 152, 1, 0x1F1F1F, 0);
    M.qrbox = mk_rect(s_page, 34, 504, 104, 104, 0x141414, 8);
    M.qr = NULL; M.net_shown = -1; M.qr_txt[0] = 0;
    M.l2 = icon_create(s_page, IC_WIFI, 86, 556, 36, 0x3A3A3A);         // shown until the QR code is ready
    M.l3 = NULL;
    s_scroll = s_scroll_v = 0; s_dragging = false; s_drag_dist = 0;
    lv_obj_add_event_cb(s_page, tasks_drag, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_page, tasks_drag, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_page, tasks_drag, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_page, tasks_drag, LV_EVENT_PRESS_LOST, NULL);
    { int lvl; store_lock(); lvl = store_settings()->task_font; store_unlock(); tf_set(lvl); }
    rows_sync(true);
    M.hop_t = -1e9f;
}

static void tick_tasks(void)
{
    char b[96];
    store_lock(); tasklist_t *tl = store_tasks(); int done = 0; for (int i = 0; i < tl->n; i++) done += tl->t[i].done; int n = tl->n; store_unlock();
    snprintf(b, sizeof b, "%d/%d", done, n); relabel(M.l1, b);
    net_mode_t nm = net_mode();
    if ((int)nm != M.net_shown) {
        M.net_shown = nm;
        icon_set(M.l2, (nm == NET_STA || nm == NET_AP) ? IC_NONE : IC_WIFI);
        if (nm == NET_STA || nm == NET_AP) {
            net_qr_text(M.qr_txt, sizeof M.qr_txt);
            lv_obj_set_style_bg_color(M.qrbox, lv_color_white(), 0);
            if (!M.qr) { M.qr = lv_qrcode_create(M.qrbox); lv_qrcode_set_size(M.qr, 96); lv_qrcode_set_dark_color(M.qr, lv_color_black()); lv_qrcode_set_light_color(M.qr, lv_color_white()); lv_obj_center(M.qr); }
            lv_qrcode_update(M.qr, M.qr_txt, strlen(M.qr_txt));
            ch_hop(2.2f);
        } else if (M.qr) {                       // Wi-Fi went off: the code would no longer work
            lv_obj_delete(M.qr); M.qr = NULL;
            lv_obj_set_style_bg_color(M.qrbox, lv_color_hex(0x141414), 0);
        }
    }
    CH.expr = s_now - M.hop_t < 1200 ? EX_HAPPY : CH.expr == EX_HAPPY && !CH.grav ? EX_NORMAL : CH.expr;
    CH.gaze_ov_on = true; CH.gaze_x = sinf(s_now / 2600) * 8; CH.gaze_y = 7;
}

static void build_clear(void)
{
    ch_reset(W, H, 86, 604, .5f);
    CH.ly0 = 570;                                        // below the badge
    { int lvl; store_lock(); lvl = store_settings()->task_font; store_unlock(); tf_set(lvl); }
    rows_sync(true);
    M.dim = mk_rect(s_page, 0, 0, W, H, 0x000000, 0);
    lv_obj_set_style_bg_opa(M.dim, 0, 0);
    // the tier badge: an icon and a count in a small group that pops in
    M.tier = lv_obj_create(s_page); lv_obj_remove_style_all(M.tier); lv_obj_set_size(M.tier, W, 48); lv_obj_set_pos(M.tier, 0, 514);
    lv_obj_remove_flag(M.tier, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(M.tier, W / 2, 0); lv_obj_set_style_transform_pivot_y(M.tier, 24, 0);
    { lv_obj_t *keep = s_page; s_page = M.tier; irow_make(&IR2, 86, 38, 1, 32, &font_num36); s_page = keep; }
    int n = 0; for (int i = 0; i < NR; i++) if (R[i].done) n++;
    M.tier_n = n; M.tier_kind = n == 0 ? 0 : n == 1 ? 1 : n <= 3 ? 2 : 3;
    M.phase = 0; M.cutk = 0; M.t1 = 0;
    // nothing checked: [list] 0 · slash: [star] · combo: [star] x3 · ultimate: [star] x5 in red
    char b[12]; snprintf(b, sizeof b, M.tier_kind == 0 ? "0" : M.tier_kind == 1 ? "" : "x%d", n);
    uint32_t tc = M.tier_kind == 3 ? C_RED : M.tier_kind == 0 ? 0x8C8C8C : C_AMBER;
    { iitem_t it[1] = { { IC_STAR, b, tc } }; irow_set(&IR2, M.tier_kind == 0 ? 0 : 1, it); }   // nothing checked: no badge, the sad face says it
    lv_obj_set_style_opa(M.tier, 0, 0);
    M.tier_t = s_now + 520;
    int k = 0;
    for (int i = 0; i < NR; i++) if (R[i].done) {
        R[i].cut = true; R[i].cut_dir = k % 2 ? -1 : 1;
        if (M.tier_kind == 3) { R[i].cut_big = true; R[i].cut_t0 = s_now + 1500; }
        else R[i].cut_t0 = s_now + 800 + k * 330;
        k++;
    }
    M.end = M.tier_kind == 0 ? 0 : M.tier_kind == 3 ? s_now + 2400 : s_now + 800 + (n - 1) * 330 + 800;
}

// The ultimate's darkening covers the whole screen, so every change of it repaints and resends
// everything. It moves in steps of 1/10 instead of every frame: it still reads as a fade.
static void dim_to(int v)
{
    // The dimming veil only needs to cover what is drawn on the page: black under it stays black.
    // Fitting it to the rows (instead of the whole screen) makes every dimming step repaint less.
    if (v > 0 && !M.dim_fit) {
        M.dim_fit = true;
        lv_area_t u = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_page); i++) {
            lv_obj_t *c = lv_obj_get_child(s_page, i);
            if (c == M.dim || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_area_t a; lv_obj_get_coords(c, &a);
            u.x1 = LV_MIN(u.x1, a.x1); u.y1 = LV_MIN(u.y1, a.y1); u.x2 = LV_MAX(u.x2, a.x2); u.y2 = LV_MAX(u.y2, a.y2);
        }
        if (u.x1 <= u.x2) {
            lv_area_t pg; lv_obj_get_coords(s_page, &pg);
            int x = LV_MAX(0, u.x1 - pg.x1 - 8), y = LV_MAX(0, u.y1 - pg.y1 - 8);
            lv_obj_set_pos(M.dim, x, y);
            lv_obj_set_size(M.dim, LV_MIN(W, u.x2 - pg.x1 + 9) - x, LV_MIN(H, u.y2 - pg.y1 + 9) - y);
        }
    }
    if (v > M.dimv + 0.5f || v < M.dimv - 0.5f) M.dimv = v;
    int q = (v + 7) / 15 * 15; if (q > 150) q = 150;
    if (lv_obj_get_style_bg_opa(M.dim, 0) != q) lv_obj_set_style_bg_opa(M.dim, (lv_opa_t)q, 0);
}

static void tick_clear(void)
{
    float e = s_now - s_t0;
    // tier label pop
    float te = s_now - M.tier_t;
    if (te >= 0) {
        float a = te < 200 ? te / 200 : te > 2600 ? fmaxf(0, 1 - (te - 2600) / 400) : 1;
        set_opa(M.tier, (lv_opa_t)(255 * a));
        { int32_t sc = (int32_t)(256 * (te < 200 ? 1.3f - .3f * te / 200 : 1)); set_scale(M.tier, sc, sc); }
    }
    if (M.tier_kind == 0) { CH.expr = EX_SAD; CH.tilt_ov_on = true; CH.tilt_ov = sinf(e / 280) * 14; CH.gaze_ov_on = true; CH.gaze_x = sinf(e / 280) * 8; CH.gaze_y = -4; return; }
    if (M.phase == 0) {
        CH.expr = EX_DETERMINED; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = -9; CH.tilt_ov_on = true; CH.tilt_ov = -12;
        if (M.tier_kind == 3 && e > 500) { dim_to((int)(150 * fminf(1, (e - 500) / 600))); CH.scale_t = CH.base * 1.45f; CH.expr = EX_STRAIN; CH.tilt_ov = sinf(e / 40) * 4; }
        float first = M.tier_kind == 3 ? 1360 : 800;
        if (e > first) {
            M.phase = 1;
            s_shake_until = s_now + (M.tier_kind == 3 ? 380 : M.tier_kind == 2 ? 160 : 110); s_shake_amp = M.tier_kind == 3 ? 5 : M.tier_kind == 2 ? 3 : 2;
            if (M.tier_kind == 3) { s_flash_t = s_now; ch_burst_stars(); }
        }
    } else if (M.phase == 1) {
        CH.expr = EX_STRAIN;
        int k = (int)((e - 800) / 330);
        CH.tilt_ov = M.tier_kind == 3 ? 0 : (k % 2 ? 14 : -14);
        if (M.tier_kind == 2 && k > M.cutk) { M.cutk = k; s_shake_until = s_now + 140; s_shake_amp = 3; }
        if (M.tier_kind == 3) { M.dimv = fmaxf(0, M.dimv - 6); dim_to((int)M.dimv); CH.scale_t = CH.base; }
        if (s_now > M.end) {
            M.phase = 2; M.t1 = s_now;
            int n = store_task_clear_done();
            for (int i = 0; i < NR; i++) if (R[i].cut) {
                for (int h = 0; h < 2; h++) if (R[i].halves[h]) lv_obj_delete(R[i].halves[h]);
                lv_obj_delete(R[i].o); R[i] = R[--NR]; i--;
            }
            rows_sync(false);
            ch_hop(3); ch_burst_stars();
            char b[12]; snprintf(b, sizeof b, "%d", n);
            { iitem_t it[1] = { { IC_CHECK, b, C_MINT } }; irow_set(&IR2, 1, it); }      // [tick] 3 cleared
            M.tier_t = s_now;
            s_seen_ver = store_version();
        }
    } else {
        CH.expr = EX_HAPPY; CH.tilt_ov_on = false; CH.gaze_ov_on = true; CH.gaze_x = 0; CH.gaze_y = -2;
        if (lv_obj_get_style_bg_opa(M.dim, 0)) lv_obj_set_style_bg_opa(M.dim, 0, 0);
    }
}

static float s_house_x = 262, s_house_y = 83;
static void build_carry(void)
{
    // landscape: house and clock side by side; stood up (Tasks / Clear): house above the clock
    bool land = W > H;
    s_house_x = land ? 262 : W / 2; s_house_y = land ? 83 : H / 2 - 23;
    ch_reset(W, H, land ? 112 : W / 2, s_house_y, 1);
    CH.alpha = 0;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    char hm[8] = "--:--"; if (rtc_time_valid()) snprintf(hm, sizeof hm, "%02d:%02d", lt.tm_hour, lt.tm_min);
    if (land) M.l1 = mk_label(s_page, hm, &font_num36, 0xA8A8A8, 308, 99, 0);   // small: in a bag nobody reads it
    else M.l1 = mk_label(s_page, hm, &font_num36, 0xA8A8A8, W / 2, H / 2 + 52, 1);
}

static void tick_carry(void)
{
    if (rtc_time_valid()) { time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt); char hm[8]; snprintf(hm, sizeof hm, "%02d:%02d", lt.tm_hour, lt.tm_min); relabel(M.l1, hm); }
}

static void settings_btn(lv_event_t *e) { app_settings_step((int)(intptr_t)lv_event_get_user_data(e)); }
static void settings_tab(lv_event_t *e) { app_settings_set_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void cal_btn(lv_event_t *e) { app_cal_start(); }
static int s_slide_dir;                // -1 slid in from the right, +1 from the left, 0 none
static void tx_cb(void *obj, int32_t v) { lv_obj_set_style_translate_x(obj, v, 0); }
void ui_settings_swipe(bool left)
{
    if (s_scr != SCR_SETTINGS) return;
    int pg = app_settings_page();
    if (left && pg < 2) { s_slide_dir = -1; app_settings_set_page(2); }
    else if (!left && pg >= 2) { s_slide_dir = 1; app_settings_set_page(0); }
}
// Swipe left / right between the timer screen and the device screen. Measured from where the finger
// went down to where it came up, so it doesn't depend on LVGL's gesture speed rules.
static lv_point_t s_sw0;
static void settings_gesture(lv_event_t *e)
{
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { s_sw0 = p; return; }
    int dx = p.x - s_sw0.x, dy = p.y - s_sw0.y;
    if (abs(dx) > 50 && abs(dx) > 2 * abs(dy)) ui_settings_swipe(dx < 0);
}
static void cal_reset_btn(lv_event_t *e) { app_cal_reset(); }

#define C_LIGHT 0xFFE08A
#define BR_X0 150
#define BR_X1 450
#define BR_Y  158
static lv_obj_t *s_br_fill, *s_br_knob;
static void bright_drag(lv_event_t *e)
{
    lv_point_t pt; lv_indev_get_point(lv_indev_active(), &pt);
    float f = (pt.x - BR_X0) / (float)(BR_X1 - BR_X0);
    int pct = 10 + (int)lroundf(fminf(1, fmaxf(0, f)) * 90);
    if (pct != app_brightness_pct()) app_set_brightness_pct(pct);
}

static void settings_buttons(int pg)
{
    static const uint32_t cols[4] = { C_AMBER, C_SKY, 0xFFFFFF, C_LIGHT };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = mk_rect(s_page, (i ? 518 : 82) - 34, 52, 68, 68, 0x0D0D0D, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(b, 2, 0); lv_obj_set_style_border_color(b, lv_color_hex(0x4A4A4A), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, settings_btn, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 1 : -1));
        lv_obj_t *keep = s_page; s_page = b; icon_create(s_page, i ? IC_PLUS : IC_MINUS, 32, 32, 30, 0xFFFFFF); s_page = keep;   /* 34 minus the 2 px ring: children sit inside it */
    }
    (void)cols; (void)pg;
}

static void build_settings(void)
{
    ch_reset(W, H, 40, 146, .36f);                       // small, bottom-left: the numbers get the space
    int pg = app_settings_page();
    settings_t st; store_lock(); st = *store_settings(); store_unlock();
    // two screens: timers (study, break) and device (brightness, direction, swipe left to reach it)
    // tabs are icons: study [book] / break [cup] on the timer screen, [sun] / [rotate] on the device screen
    static const icon_t tabs[4] = { IC_BOOK, IC_CUP, IC_SUN, IC_ROTATE };
    static const uint32_t cols[4] = { C_AMBER, C_SKY, C_LIGHT, 0xFFFFFF };
    // the two tabs stack in a narrow column on the right edge, so the number can use the full height
    static const int ty[4] = { 54, 122, 54, 122 };
    int scr0 = pg < 2 ? 0 : 2;
    for (int i = scr0; i < scr0 + 2; i++) {
        lv_obj_t *l = icon_create(s_page, tabs[i], 608, ty[i], 30, i == pg ? cols[i] : 0x3A3A3A);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(l, 14);
        lv_obj_add_event_cb(l, settings_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i == pg) mk_rect(s_page, 574, ty[i] - 17, 4, 34, cols[i], 2);
    }
    ND = 0;
    if (pg <= 1) {
        char v[4]; snprintf(v, sizeof v, "%02d", pg ? st.break_min : st.study_min);
        make_digits(v, &font_num150, 300, 86, cols[pg]);                // the minutes, as big as the screen allows
        if (pg == 1 && v[0] == '0') { D[0].col = 0x2C4A73; digit_char(&D[0], '0'); }
        settings_buttons(pg);
    } else if (pg == 3) {
        // [rotate] big, a tick when calibrated, a round start button, and reset
        bool ok = app_cal_valid();
        icon_create(s_page, IC_ROTATE, 230, 104, 64, 0xFFFFFF);
        if (ok) icon_create(s_page, IC_CHECK, 276, 70, 26, C_MINT);
        lv_obj_t *bt = mk_rect(s_page, 330, 78, 88, 52, 0x0D0D0D, 26);
        lv_obj_set_style_border_width(bt, 2, 0); lv_obj_set_style_border_color(bt, lv_color_white(), 0);
        lv_obj_add_flag(bt, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(bt, 10);
        lv_obj_add_event_cb(bt, cal_btn, LV_EVENT_CLICKED, NULL);
        { lv_obj_t *keep = s_page; s_page = bt; icon_create(s_page, IC_PLAY, 42, 24, 26, 0xFFFFFF); s_page = keep; }   /* inside the 2 px ring */
        if (ok) {
            lv_obj_t *r = mk_rect(s_page, 440, 78, 52, 52, 0x0D0D0D, 26);
            lv_obj_set_style_border_width(r, 2, 0); lv_obj_set_style_border_color(r, lv_color_hex(0x4A4A4A), 0);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(r, 8);
            lv_obj_add_event_cb(r, cal_reset_btn, LV_EVENT_CLICKED, NULL);
            lv_obj_t *keep = s_page; s_page = r; icon_create(s_page, IC_RESET, 24, 24, 26, 0xA8A8A8); s_page = keep;
        }
    } else {
        int pct = app_brightness_pct();
        char v[4]; snprintf(v, sizeof v, "%d", pct);
        make_digits(v, &font_num150, 292, 78, C_LIGHT);
        lv_font_glyph_dsc_t g; lv_font_get_glyph_dsc(&font_num150, &g, '0', 0);
        mk_label(s_page, "%", &font_num36, 0xCFCFCF, 292 + (int)(g.adv_w * strlen(v) / 2) + 14, 128, 0);   // room for the digits to squash
        settings_buttons(pg);
        mk_rect(s_page, BR_X0, BR_Y - 3, BR_X1 - BR_X0, 6, 0x262626, 3);
        s_br_fill = mk_rect(s_page, BR_X0, BR_Y - 3, 1, 6, C_LIGHT, 3);
        s_br_knob = mk_rect(s_page, BR_X0 - 10, BR_Y - 10, 20, 20, 0xFFFFFF, LV_RADIUS_CIRCLE);
        lv_obj_t *hit = mk_rect(s_page, BR_X0 - 16, BR_Y - 22, BR_X1 - BR_X0 + 32, 38, 0x000000, 0);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, bright_drag, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(hit, bright_drag, LV_EVENT_PRESSING, NULL);
        lv_obj_remove_flag(hit, LV_OBJ_FLAG_GESTURE_BUBBLE);     // dragging the bar isn't a swipe

        M.sec = pct;
    }
    lv_obj_add_event_cb(s_page, settings_gesture, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_page, settings_gesture, LV_EVENT_RELEASED, NULL);
    if (s_slide_dir) {                     // the new screen slides in from the side you swiped toward
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, s_page); lv_anim_set_exec_cb(&a, tx_cb);
        lv_anim_set_values(&a, -s_slide_dir * 90, 0); lv_anim_set_duration(&a, 240); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        s_slide_dir = 0;
    }
    M.last_page = pg;
}

static void tick_settings(void)
{
    float dir = ND && D[0].x < CH.x ? -1 : 1;
    CH.gaze_ov_on = true; CH.gaze_x = dir * 12; CH.gaze_y = -1;
    CH.expr = s_now - M.press_t < 1200 ? EX_HAPPY : EX_NORMAL;
    if (app_settings_page() == 2 && s_br_fill) {
        int pct = app_brightness_pct();
        int w = (int)((BR_X1 - BR_X0) * (pct - 10) / 90.f);
        lv_obj_set_width(s_br_fill, w < 1 ? 1 : w);
        lv_obj_set_x(s_br_knob, BR_X0 + w - 10);
        if (pct != M.sec) {
            char v[4]; snprintf(v, sizeof v, "%d", pct);
            if ((int)strlen(v) != ND) { ui_show(SCR_SETTINGS, app_orient(), 0); return; }   // 2 <-> 3 digits
            digits_set(v, false); M.sec = pct;
        }
        // the character squints in dim light and beams in bright light
        CH.expr = pct < 35 ? EX_FOCUS : pct > 85 ? EX_HAPPY : EX_NORMAL;
    }
}

void ui_settings_changed(int dir)
{
    if (s_scr != SCR_SETTINGS) return;
    int pg = app_settings_page();
    if (pg >= 2) { M.press_t = s_now; return; }          // brightness updates itself every frame; direction has no stepper
    settings_t st; store_lock(); st = *store_settings(); store_unlock();
    char v[4]; snprintf(v, sizeof v, "%02d", pg ? st.break_min : st.study_min);
    // roll changed digits, keeping the page (rebuild dots without touching the digits)
    char old[3] = { D[0].ch[0], D[1].ch[0], 0 };
    if (!strcmp(old, v)) { s_shake_until = s_now + 120; s_shake_amp = 2; return; }
    P.roll = true; memcpy(P.old, old, 3); snprintf(P.val, sizeof P.val, "%s", v);
    ui_show(SCR_SETTINGS, app_orient(), 0);
}

// ---------------------------------------------------------------- direction calibration screen
// Drawn already turned the way that mode is shown; you turn the device until it reads upright.
static void build_cal(void)
{
    // [mode icon] 1/4, a big arrow that must point up, and a bar that fills while you hold still
    static const icon_t mode[4] = { IC_HOME, IC_BOOK, IC_TASKS, IC_SWEEP };
    int k = app_cal_step() - 1; if (k < 0) k = 0;
    char st[8]; snprintf(st, sizeof st, "%d/4", k + 1);
    iitem_t it[1] = { { mode[k], st, C_AMBER } };
    if (W > H) {
        // the character sits where it lives in that mode: left on Home, right on Study
        float cx = k == 0 ? 90 : 550, tx = k == 0 ? 400 : 240;
        ch_reset(W, H, cx, 80, .7f);
        irow_make(&IR1, tx - 110, 96, 1, 34, &font_num28); irow_set(&IR1, 1, it);
        icon_create(s_page, IC_UP, tx + 40, 80, 96, 0xFFFFFF);
        mk_rect(s_page, tx - 150, 150, 300, 8, 0x262626, 4);
        M.bar = mk_rect(s_page, tx - 150, 150, 1, 8, C_AMBER, 4);
    } else {
        // top end on Tasks, bottom end on Clear
        float cy = k == 2 ? 70 : H - 70, ty = k == 2 ? 190 : 170;
        ch_reset(W, H, 86, cy, .6f);
        irow_make(&IR1, 86, ty, 1, 34, &font_num28); irow_set(&IR1, 1, it);
        icon_create(s_page, IC_UP, 86, ty + 110, 130, 0xFFFFFF);
        mk_rect(s_page, 16, ty + 200, 140, 8, 0x262626, 4);
        M.bar = mk_rect(s_page, 16, ty + 200, 1, 8, C_AMBER, 4);
    }
    M.press_t = -1e9f;
}

static void tick_cal(void)
{
    float p = app_cal_progress();
    int full = W > H ? 300 : 140;
    lv_obj_set_width(M.bar, (int)fmaxf(1, full * p));
    if (s_now - M.press_t < 700) { CH.expr = EX_HAPPY; return; }
    CH.gaze_ov_on = false;
    CH.expr = p > 0.05f ? EX_FOCUS : EX_NORMAL;
}

void ui_cal_captured(void)
{
    if (s_scr != SCR_CAL) return;
    M.press_t = s_now;
    lv_obj_set_width(M.bar, W > H ? 300 : 140);
    lv_obj_set_style_bg_color(M.bar, lv_color_hex(C_MINT), 0);
    ch_hop(2.4f); ch_burst_stars();
}

void ui_knock(void) { s_knock_t = s_now; }

// ================================================================== overlay drawing (character, battery, effects)
static void draw_battery(lv_layer_t *layer)
{
    int pct = power_battery_pct();
    bool low = app_low_battery(), usb = power_on_usb();
    uint32_t col = low ? C_AMBER : usb ? C_MINT : 0x5A5A5A;    // small and quiet; amber only when low
    int bw = 18, bx = W - 12 - bw, by = 7;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_opa = 0; r.border_color = lv_color_hex(col); r.border_width = 1; r.radius = 2;
    lv_area_t a = { bx, by, bx + bw, by + 9 }; lv_draw_rect(layer, &r, &a);
    lv_draw_rect_dsc_init(&r); r.bg_color = lv_color_hex(col); r.radius = 1;
    float f = pct < 0 ? .5f : pct / 100.f;
    lv_area_t b = { bx + 2, by + 2, bx + 2 + (int)fmaxf(1, (bw - 4) * f), by + 7 }; lv_draw_rect(layer, &r, &b);
    lv_area_t c = { bx + bw + 1, by + 3, bx + bw + 2, by + 6 }; lv_draw_rect(layer, &r, &c);
}

static void draw_house(lv_layer_t *layer, float d)
{
    // the little shed the character hides in (carry mode)
    float x = s_house_x, y = s_house_y, k = .5f;   // roof to floor centred on the screen
    #define HX(v) (x + (v) * k)
    #define HY(v) (y + (v) * k)
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l); l.color = lv_color_hex(0xCFCFCF); l.width = 2; l.round_start = l.round_end = 1;
    l.p1.x = HX(-68); l.p1.y = HY(-6); l.p2.x = HX(0); l.p2.y = HY(-46); lv_draw_line(layer, &l);
    l.p1.x = HX(0); l.p1.y = HY(-46); l.p2.x = HX(68); l.p2.y = HY(-6); lv_draw_line(layer, &l);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(0x0D0D0D); r.border_color = lv_color_hex(0xCFCFCF); r.border_width = 1; r.radius = 2;
    lv_area_t body = { (int)HX(-56), (int)HY(-10), (int)HX(56), (int)HY(60) }; lv_draw_rect(layer, &r, &body);
    r.bg_color = lv_color_hex(0x4A3A1C); r.border_color = lv_color_hex(0xFFD27A);
    lv_area_t win = { (int)HX(-44), (int)HY(4), (int)HX(-10), (int)HY(30) }; lv_draw_rect(layer, &r, &win);
    r.bg_color = lv_color_hex(0x1C1C1C); r.border_color = lv_color_hex(0xCFCFCF);
    lv_area_t door = { (int)HX(12), (int)HY(14), (int)HX(38), (int)HY(60) }; lv_draw_rect(layer, &r, &door);
    // eyes peeking from the window now and then
    float c = fmodf(d, 4500);
    if (c > 2500 && c < 4200) {
        bool blink = fmodf(d, 3200) < 160;
        float gl = sinf(d / 900) * 2, h = blink ? 1.5f : 9 * k;
        lv_draw_rect_dsc_init(&r); r.bg_color = lv_color_white(); r.radius = 2;
        for (int i = 0; i < 2; i++) {
            float ex = HX(-31 + i * 10 + gl);
            lv_area_t e = { (int)ex, (int)(HY(16.5f) - h / 2), (int)ex + 2, (int)(HY(16.5f) + h / 2) }; lv_draw_rect(layer, &r, &e);
        }
    }
    // chimney smoke
    for (int i = 0; i < 2; i++) {
        float q = fmodf(d / 5000 + i * .5f, 1);
        lv_draw_rect_dsc_init(&r); r.bg_color = lv_color_hex(0x4A4A4A); r.bg_opa = (lv_opa_t)(sinf(q * (float)M_PI) * 200); r.radius = LV_RADIUS_CIRCLE;
        float cx = HX(40 + q * 8), cy = HY(-44 - q * 26), rad = (5 + i * 2) * k;
        lv_area_t s = { (int)(cx - rad), (int)(cy - rad), (int)(cx + rad), (int)(cy + rad) }; lv_draw_rect(layer, &r, &s);
    }
    #undef HX
    #undef HY
}

static void draw_slashes(lv_layer_t *layer)
{
    bool big_done = false;
    for (int i = 0; i < NR; i++) {
        row_t *r = &R[i];
        if (!r->cut) continue;
        float e = s_now - r->cut_t0;
        if (e < 0) continue;
        if (r->cut_big) {
            if (e > 700 || big_done) continue;
            big_done = true;
        } else if (e > 520) continue;
        float p = clampf(e / (r->cut_big ? 160 : 140), 0, 1), fade = r->cut_big ? (e > 200 ? 1 - (e - 200) / 500 : 1) : (e > 260 ? 1 - (e - 260) / 260 : 1);
        float x0, y0, x1, y1, w;
        if (r->cut_big) {
            float ya = 1e9f, yb = -1e9f; for (int k = 0; k < NR; k++) if (R[k].cut) { ya = fminf(ya, R[k].ty); yb = fmaxf(yb, R[k].ty + RH); }
            x0 = -10; y0 = yb + 40; x1 = 182; y1 = ya - 30; w = 9;
        } else {
            x0 = r->cut_dir > 0 ? 8 : 164; y0 = r->y + RH + 4; x1 = r->cut_dir > 0 ? 164 : 8; y1 = r->y + 2; w = r->cut_big ? 9 : 3.5f;
        }
        float xe = x0 + (x1 - x0) * p, ye = y0 + (y1 - y0) * p;
        if (lroundf(x0) == lroundf(xe) && lroundf(y0) == lroundf(ye)) continue;   // LVGL 9.2 zero-length line bug
        lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l); l.round_start = l.round_end = 1;
        l.p1.x = lroundf(x0); l.p1.y = lroundf(y0); l.p2.x = lroundf(xe); l.p2.y = lroundf(ye);
        l.width = (int32_t)(w * 2.6f); l.color = lv_color_hex(r->cut_big ? C_RED : C_AMBER); l.opa = (lv_opa_t)(90 * fade); lv_draw_line(layer, &l);
        l.width = (int32_t)w; l.color = lv_color_white(); l.opa = (lv_opa_t)(255 * fade); lv_draw_line(layer, &l);
    }
}


#define APART(p) 1   // every overlay part is drawn
static void overlay_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_scr == SCR_CARRY && APART(AP_HOUSE)) draw_house(layer, s_now - s_t0);
    if (s_scr == SCR_CLEAR && APART(AP_FX)) draw_slashes(layer);
    if (s_scr == SCR_HOME && APART(AP_RAIN)) rain_draw(layer);
    if (APART(AP_CHAR) && (!s_ch_box_ok || clip_hit(layer, s_ch_box.x1, s_ch_box.y1, s_ch_box.x2, s_ch_box.y2))) ch_draw(layer);
    if (APART(AP_BATT) && clip_hit(layer, W - 36, 2, W - 6, 22)) draw_battery(layer);
    if (!APART(AP_FX)) return;
    // knock ripples
    float ke = s_now - s_knock_t;
    if (ke >= 0 && ke < 1400) {
        const float off[3] = { 0, 200, 700 };
        for (int i = 0; i < 3; i++) {
            float q = (ke - off[i]) / 650;
            if (q <= 0 || q >= 1) continue;
            lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
            a.center.x = W / 2; a.center.y = H / 2; a.radius = (uint16_t)(8 + q * 70); a.start_angle = 0; a.end_angle = 360;
            a.width = 2; a.color = lv_color_white(); a.opa = (lv_opa_t)(255 * (1 - q));
            lv_draw_arc(layer, &a);
        }
    }
    float fe = s_now - s_flash_t;
    if (fe >= 0 && fe < 260) {
        lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r); r.bg_color = lv_color_white(); r.bg_opa = (lv_opa_t)(217 * (1 - floorf(fe / 33.3f) * 33.3f / 260));
        lv_area_t a = { 0, 0, W, H }; lv_draw_rect(layer, &r, &a);
    }
}

// ================================================================== show / loop
static bool s_long_fired;
static float s_last_tap = -1e9f;
static bool s_tap_pending;          // single tap in Study waits 400 ms in case it becomes a double-tap
static void page_long(lv_event_t *e)
{
    // Hold ~1 s on Study, Break, or Home while a study is paused to end the session.
    app_activity();
    s_tap_pending = false;
    if (s_scr == SCR_STUDY || s_scr == SCR_BREAK || s_scr == SCR_HOME) s_long_fired = app_end_session();
}

static void page_click(lv_event_t *e)
{
    app_activity();
    if (s_long_fired) { s_long_fired = false; return; }
    if (s_scr == SCR_DONE) { app_done_skip(); return; }
    if (s_scr == SCR_STUDY) {                       // tap = pause/resume, double-tap = restart the countdown
        if (s_now - s_last_tap < 400) { s_last_tap = -1e9f; s_tap_pending = false; app_study_reset(); return; }
        s_last_tap = s_now; s_tap_pending = true;
    }
    // SCR_BREAK: taps do nothing, so a break can't be extended
}

static void tap_check(void)
{
    if (!s_tap_pending || s_now - s_last_tap < 400) return;
    s_tap_pending = false;
    if (s_scr == SCR_STUDY) app_study_toggle_pause();
}

void ui_study_pause_fx(bool paused)
{
    if (s_scr != SCR_STUDY) return;
    M.press_t = s_now;
    CH.expr = paused ? EX_NORMAL : EX_DETERMINED;
    ch_hop(paused ? 1.2f : 2.0f);
}

void ui_study_reset_fx(void)
{
    if (s_scr != SCR_STUDY) return;
    session_t ss; store_lock(); ss = *store_session(); store_unlock();
    int s = (ss.study_left_ms + 999) / 1000;
    char b[8]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    M.sec = s;
    digits_set(b, true);
    CH.expr = EX_SURPRISED; ch_hop(2.6f);
    M.press_t = s_now;
}

// The page fading in after a turn: every opacity step repaints the whole page, so it moves in 12
// even steps (about every other frame) rather than a new value every frame. A fade from black
// looks the same; the character and digits keep moving at full speed meanwhile.
static void opa_cb(void *obj, int32_t v) { int q = v >= 250 ? 255 : (v + 10) / 21 * 21; set_opa(obj, (lv_opa_t)q); }

static float s_show_t = -1e9f;
static void do_show(screen_t s, orient_t o, float turn)
{
    // Settings is laid out for the wide screen: when the device is stood up, it shows lying down
    if (s == SCR_SETTINGS && (o == ORI_P || o == ORI_PF)) o = ORI_L;
    s_show_t = s_now;
    display_set_orientation(o);
    float old_x = CH.x, old_y = CH.y; int old_w = W, old_h = H;
    screen_t prev = s_scr;
    s_scr = s;
    lv_display_t *disp = lv_display_get_default();
    W = lv_display_get_horizontal_resolution(disp);
    H = lv_display_get_vertical_resolution(disp);
    if (s_page) lv_obj_delete(s_page);
    rows_clear_all();
    memset(&M, 0, sizeof M);
    ND = 0; G.l = NULL; G.on = false;
    memset(&IR1, 0, sizeof IR1); memset(&IR2, 0, sizeof IR2); memset(&IR3, 0, sizeof IR3);   // no stale pointers into the old page
    s_page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, W, H);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_GESTURE_BUBBLE);   // swipes stop here (Settings), not at the screen behind
    lv_obj_add_event_cb(s_page, page_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_page, page_long, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_size(s_overlay, W, H);
    s_t0 = s_now;
    switch (s) {
    case SCR_HOME: build_home(); if (prev == SCR_BREAK) { M.woke = true; M.wake_t = s_now; } break;
    case SCR_STUDY: build_study(); break;
    case SCR_DONE: build_done(); break;
    case SCR_BREAK: build_break(); break;
    case SCR_TASKS: build_tasks(); break;
    case SCR_CLEAR: build_clear(); break;
    case SCR_CARRY: build_carry(); break;
    case SCR_SETTINGS: build_settings(); break;
    case SCR_CAL: build_cal(); break;
    }
    s_seen_ver = store_version();
    if (turn != 0) {
        // the character keeps its physical spot: map the old position through the turn, then spin upright
        float a = turn * (float)M_PI / 180, dx = old_x - old_w / 2.f, dy = old_y - old_h / 2.f;
        CH.x = W / 2.f + dx * cosf(a) - dy * sinf(a);
        CH.y = H / 2.f + dx * sinf(a) + dy * cosf(a);
        ch_spin_in(turn);
        digits_drop_in(380);
        lv_obj_set_style_opa(s_page, 0, 0);
        lv_anim_t an; lv_anim_init(&an); lv_anim_set_var(&an, s_page); lv_anim_set_values(&an, 0, 255);
        lv_anim_set_duration(&an, 360); lv_anim_set_delay(&an, 380); lv_anim_set_exec_cb(&an, opa_cb); lv_anim_start(&an);
        for (int i = 0; i < NR; i++) R[i].a = -1.3f;
    }
}

void ui_show(screen_t s, orient_t o, float turn)
{
    P.on = true; P.s = s; P.o = o;
    if (turn != 0) P.turn = turn;
}

void ui_show_now(screen_t s, orient_t o)
{
    P.on = false; P.turn = 0;
    do_show(s, o, 0);
    lv_refr_now(NULL);
}

static void run_pending(void)
{
    if (!P.on) return;
    P.on = false;
    float turn = P.turn; P.turn = 0;
    do_show(P.s, P.o, turn);
    if (P.roll && s_scr == SCR_SETTINGS && ND >= 2) {
        P.roll = false;
        for (int i = 0; i < 2; i++) digit_char(&D[i], P.old[i]);
        digits_set(P.val, true);
        M.press_t = s_now; ch_hop(2.2f);
    }
}

static lv_obj_t *s_msg_ic[2];
void ui_toast(icon_t a, icon_t b)
{
    icon_set(s_msg_ic[0], a); icon_set(s_msg_ic[1], b);
    int n = (a != IC_NONE) + (b != IC_NONE);
    lv_obj_set_size(s_msg, n == 2 ? 150 : 84, 84);
    lv_obj_set_pos(s_msg_ic[0], 42 - lv_obj_get_width(s_msg_ic[0]) / 2, 42 - lv_obj_get_height(s_msg_ic[0]) / 2);
    lv_obj_set_pos(s_msg_ic[1], 108 - lv_obj_get_width(s_msg_ic[1]) / 2, 42 - lv_obj_get_height(s_msg_ic[1]) / 2);
    lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, 0);
}
void ui_message(const char *txt) { if (!txt) lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN); }
static void msg_hide(lv_timer_t *t) { lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN); lv_timer_delete(t); }
void ui_message_hide_after(int ms) { lv_timer_create(msg_hide, ms, NULL); }

static bool rain_visible(void)
{
    if (RN.amt > .02f) return true;
    for (int j = 0; j < NSPL; j++) if (RN.s[j].life > 0) return true;
    return false;
}

// Only redraw what the overlay changed: full screen while something screen-wide is animating
// (rain, slashes, carry scene, knock ripples, flash, a screen change), otherwise just the area
// around the character, and only when the character actually looks different.
static lv_area_t s_ov_prev; static bool s_ov_prev_ok, s_ov_full_prev; static uint32_t s_ov_sig; static int s_batt_n;
void lv_inv_area(lv_display_t *disp, const lv_area_t *area_p);   /* LVGL internal: clear (NULL) or add an area */
static void inv(const lv_area_t *a)
{
    lv_area_t c = { LV_MAX(a->x1, 0), LV_MAX(a->y1, 0), LV_MIN(a->x2, W - 1), LV_MIN(a->y2, H - 1) };
    if (c.x1 <= c.x2 && c.y1 <= c.y2) lv_obj_invalidate_area(s_overlay, &c);
}
// Redraw only what changed: the boxes the character covered last frame and covers now (measured
// exactly), rain everywhere except behind the clock, the house in carry mode. Full-screen redraws
// are kept for the short moments that really move everything (screen changes, knocks, Clear).
// The box the slashes and knock rings cover right now. Only that is repainted for them; the whole
// screen used to be repainted (and sent) for the entire clearing animation.
static lv_area_t s_fx_prev; static bool s_fx_prev_ok;
static bool fx_bounds(lv_area_t *o, float ke)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (ke >= 0 && ke < 1500) { x0 = W / 2 - 82; y0 = H / 2 - 82; x1 = W / 2 + 82; y1 = H / 2 + 82; }
    if (s_scr == SCR_CLEAR) for (int i = 0; i < NR; i++) {
        row_t *r = &R[i];
        if (!r->cut) continue;
        float e = s_now - r->cut_t0;
        if (e < 0 || e > (r->cut_big ? 760 : 580)) continue;
        float ax, ay, bx, by, w;
        if (r->cut_big) {
            float ya = 1e9f, yb = -1e9f; for (int k = 0; k < NR; k++) if (R[k].cut) { ya = fminf(ya, R[k].ty); yb = fmaxf(yb, R[k].ty + RH); }
            ax = -10; ay = yb + 40; bx = 182; by = ya - 30; w = 9 * 1.6f;
        } else { ax = 8; bx = 164; ay = r->y + RH + 4; by = r->y + 2; w = 3.5f * 1.6f; }
        x0 = fminf(x0, fminf(ax, bx) - w - 3); x1 = fmaxf(x1, fmaxf(ax, bx) + w + 3);
        y0 = fminf(y0, fminf(ay, by) - w - 3); y1 = fmaxf(y1, fmaxf(ay, by) + w + 3);
    }
    if (x0 > x1) return false;
    o->x1 = (int32_t)fmaxf(0, x0); o->y1 = (int32_t)fmaxf(0, y0); o->x2 = (int32_t)fminf(W - 1, x1); o->y2 = (int32_t)fminf(H - 1, y1);
    return o->x1 <= o->x2 && o->y1 <= o->y2;
}

#ifndef SHOW_FULL_MS
#define SHOW_FULL_MS 40
#endif
static void ov_invalidate(void)
{
    if (!s_overlay) return;
    float ke = s_now - s_knock_t, fe = s_now - s_flash_t;
    // the white flash steps down every 33 ms: repaint the screen only when it steps (and once when it ends)
    static int flash_step = -1;
    int fs = (fe >= 0 && fe < 300) ? (fe < 260 ? (int)floorf(fe / 33.3f) : 8) : -1;
    bool flash_changed = fs != flash_step; flash_step = fs;
    bool full = (fs >= 0 && flash_changed) || s_now - s_show_t < SHOW_FULL_MS;   // just after a screen change everything is new anyway
    s_ch_box_ok = false;
    {   lv_area_t fx; bool have = !full && fx_bounds(&fx, ke);
        if (have) inv(&fx);
        if (s_fx_prev_ok && !full) inv(&s_fx_prev);
        s_fx_prev = fx; s_fx_prev_ok = have; }
    // rain: just the drops, where they were last frame and where they are now
    static lv_area_t rprev[RAIN_RAW]; static int nrprev;
    bool pre_ok = false, pre_have = false; lv_area_t pre_a = { 0 }; uint32_t pre_sig = 0;   // the character, if measured below
    bool rain = s_scr == SCR_HOME && rain_visible();
    if ((rain || nrprev) && !full) {
        lv_area_t bx[2 * RAIN_RAW]; int n = rain ? rain_boxes(bx) : 0, ncur = n;
        memcpy(bx + n, rprev, sizeof *bx * nrprev); n += nrprev;
        memcpy(rprev, bx, sizeof *bx * ncur); nrprev = ncur;
        // drops crossing the character are joined with it into one area, so the character (with its
        // umbrella) is drawn once a frame rather than once for every drop area it touches
        lv_area_t cb; uint32_t cs; bool ch = ch_measure(&cb, &cs); pre_ok = true; pre_have = ch; pre_a = cb; pre_sig = cs;
        if (ch) {
            if (s_ov_prev_ok) cb = (lv_area_t){ LV_MIN(cb.x1, s_ov_prev.x1), LV_MIN(cb.y1, s_ov_prev.y1), LV_MAX(cb.x2, s_ov_prev.x2), LV_MAX(cb.y2, s_ov_prev.y2) };
            bool grew = true;
            while (grew) { grew = false;
                for (int k = 0; k < n; k++) if (bx[k].x2 >= cb.x1 && bx[k].x1 <= cb.x2 && bx[k].y2 >= cb.y1 && bx[k].y1 <= cb.y2) {
                    cb = (lv_area_t){ LV_MIN(cb.x1, bx[k].x1), LV_MIN(cb.y1, bx[k].y1), LV_MAX(cb.x2, bx[k].x2), LV_MAX(cb.y2, bx[k].y2) };
                    bx[k--] = bx[--n]; grew = true; } }
            inv(&cb);
        }
        n = boxes_merge(bx, n, 20);
        for (int k = 0; k < n; k++) inv(&bx[k]);
    } else if (full) nrprev = 0;
    if (full || s_ov_full_prev) {           // one extra full frame after, to clear the last drops
        lv_inv_area(lv_display_get_default(), NULL);     // one whole-screen area replaces the smaller ones,
        lv_obj_invalidate(s_overlay);                    // so nothing is painted twice
        s_ov_full_prev = full; s_ov_prev_ok = false; s_ov_sig = 0;
        return;
    }
    static uint32_t s_carry_n;
    if (s_scr == SCR_CARRY && ++s_carry_n % 4 == 0) {   // the little house, at 15 fps: it's in a bag, and the smoke drifts slowly
        lv_area_t h = { (int32_t)s_house_x - 40, (int32_t)s_house_y - 62, (int32_t)s_house_x + 40, (int32_t)s_house_y + 36 };
        inv(&h);
    }
    uint32_t sig; lv_area_t a; bool have = pre_ok ? (a = pre_a, sig = pre_sig, pre_have) : ch_measure(&a, &sig);   // exact: any visible change alters sig
    bool same = have == s_ov_prev_ok && (!have || (a.x1 == s_ov_prev.x1 && a.y1 == s_ov_prev.y1 && a.x2 == s_ov_prev.x2 && a.y2 == s_ov_prev.y2));
    if (!same || sig != s_ov_sig) {
        if (have) inv(&a);
        if (s_ov_prev_ok) inv(&s_ov_prev);
    }
    s_ov_prev = a; s_ov_prev_ok = have; s_ov_sig = sig;
    if (have) { s_ch_box = a; s_ch_box_ok = true; }
    else { s_ch_box = (lv_area_t){ -100, -100, -99, -99 }; s_ch_box_ok = true; }   // nothing visible to draw
    if (++s_batt_n >= 60) {                // the battery icon: repainted only when it would look different
        s_batt_n = 0;
        static int last = -2;
        int pct = power_battery_pct(), now = (app_low_battery() << 9) | (power_on_usb() << 8) | (pct < 0 ? 255 : (int)((18 - 4) * pct / 100.f));
        if (now != last) { last = now; lv_area_t b = { W - 36, 2, W - 6, 22 }; inv(&b); }
    }
}


void ui_frame(void)
{
    int64_t us = esp_timer_get_time();
    float dt = s_last_us ? (us - s_last_us) / 1000.f : 16.7f;
    s_last_us = us;
    if (dt > 100) dt = 100;
    {   static const char *names[] = { "home", "study", "done", "break", "tasks", "clear", "carry", "settings", "calibrate" };
        static screen_t tagged = (screen_t)-1;
        if (s_scr != tagged) { tagged = s_scr; display_stats_tag(names[s_scr]); } }
    app_tick();
    tap_check();
    run_pending();
    if (store_version() != s_seen_ver) {
        s_seen_ver = store_version();
        { int lvl; store_lock(); lvl = store_settings()->task_font; store_unlock();
          // live on the Tasks screen (where the phone page is used); Clear picks it up next time it opens
          if (s_scr != SCR_CLEAR && tf_set(lvl) && s_scr == SCR_TASKS) rows_restyle(); }
        if (s_scr == SCR_TASKS) { rows_sync(false); M.hop_t = s_now; }
        if (s_scr == SCR_STUDY || s_scr == SCR_HOME) { /* labels refresh themselves */ }
    }
    s_acc += dt;
    while (s_acc >= 1000.f / 60) {
        s_acc -= 1000.f / 60;
        s_now += 1000.f / 60;
        switch (s_scr) {
        case SCR_HOME: tick_home(); break;
        case SCR_STUDY: tick_study(); break;
        case SCR_DONE: tick_done(); break;
        case SCR_BREAK: tick_break(); break;
        case SCR_TASKS: tick_tasks(); break;
        case SCR_CLEAR: tick_clear(); break;
        case SCR_CARRY: tick_carry(); break;
        case SCR_SETTINGS: tick_settings(); break;
        case SCR_CAL: tick_cal(); break;
        }
        if (s_now - CH.spin_t0 < 720) { CH.expr = s_now - CH.spin_t0 < 480 ? EX_SURPRISED : EX_NORMAL; CH.gaze_ov_on = false; CH.tilt_ov_on = false; CH.page_flip = -1; CH.think_on = false; }
        ch_step(s_now);
        digits_step();
        rows_step();
    }
}

// Called after LVGL has handled input (taps can change what's on screen), right before painting:
// places everything and marks exactly what needs repainting.
void ui_render_prep(void)
{
    s_frame_no++;
    digits_render();
    // shaking moves the whole page, so every new position repaints everything: change it every
    // other frame (30 times a second still reads as a shake, at half the cost)
    static int shake_n;
    if (s_now < s_shake_until) { if (!(shake_n++ & 1)) lv_obj_set_pos(s_page, (int)((frand() * 2 - 1) * s_shake_amp), (int)((frand() * 2 - 1) * s_shake_amp)); }
    else lv_obj_set_pos(s_page, 0, 0);
    lv_obj_update_layout(lv_screen_active());   // apply moves now, so their repaint areas are known before ours
    lv_obj_update_layout(lv_layer_top());
    ov_invalidate();
}

void ui_init(void)
{
    // Draw the clock digits' pictures once at start-up, so a digit seen for the first time never
    // makes an animation stutter while its picture is drawn.
    for (int k = 0; k < 22; k++) glyph_img(k < 11 ? &font_num132 : &font_num150, "0123456789:"[k % 11], 0xFFFFFF);
    for (int len = 4; len <= 17; len++) for (int dx = 0; dx <= 5; dx++) drop_img(len, dx, true);   // ~6 KB in all
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_msg = lv_obj_create(lv_layer_top());                // toast: one or two icons in a dark pill
    lv_obj_remove_style_all(s_msg);
    lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_msg, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_opa(s_msg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_msg, 24, 0);
    for (int i = 0; i < 2; i++) s_msg_ic[i] = icon_create(s_msg, IC_NONE, 0, 0, 48, 0xFFFFFF);
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    s_now = 1000;
}


