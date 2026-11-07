#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum { EX_NORMAL, EX_SURPRISED, EX_DETERMINED, EX_STRAIN, EX_SAD, EX_HAPPY, EX_SLEEP, EX_FOCUS, EX_PEEK } expr_t;

typedef struct {
    // body
    float x, y, vx, vy, pvx, pvy, tx, ty, k, c, maxv, bend, dist;
    float lx0, ly0, lx1, ly1;          // the character (with everything it carries) never leaves this box
    bool grav; float floor_y;
    float scale, scale_t, base, alpha;
    float home_x, home_y;
    float fx0, fx1, fy0, fy1;          // free box for idle wandering
    int   w, h;                        // screen size in logical pixels
    // face
    expr_t expr; float open, tilt, tv, gx, gy, imp, impv, acc;
    bool tilt_ov_on; float tilt_ov;
    bool gaze_ov_on; float gaze_x, gaze_y;
    bool idle; float breath_amp, breath_ms;
    bool talk;
    // props
    bool book; float book_a; float page_flip;   // page_flip <0 = none, else 0..1
    bool zzz;
    bool umbrella; float umb_a;        // rainy-day umbrella over the head
    float snow_cap;                    // 0..1 snow piled on the umbrella
    bool think_on; float think_e; bool think_bang; bool think_left;
    float stars_t;                     // time of the last burst, <0 none
    // spin used by turn animations
    float spin_from, spin_t0;
    // output: rotated bounding box (logical px)
    float bx0, bx1, by0, by1;
} character_t;

extern character_t CH;

void ch_reset(int screen_w, int screen_h, float home_x, float home_y, float scale);
void ch_free_box(float x0, float x1, float y0, float y1);
void ch_seek(float x, float y, float k, float maxv);
void ch_hop(float v);
void ch_burst_stars(void);
void ch_spin_in(float degrees);        // starts rotated by `degrees` and springs upright
void ch_step(float now_ms);            // fixed 60 Hz step
void ch_draw(lv_layer_t *layer);
uint32_t ch_signature(void);           // changes whenever the drawn character would look different
bool ch_measure(lv_area_t *out, uint32_t *print);   // the box ch_draw would paint now, and a fingerprint of it
void ch_eye_box(float *x0, float *y0, float *x1, float *y1);   // the eyes alone
bool ch_umbrella_hit(float x, float y);   // is this screen point on (or under) the umbrella canopy?
void ch_umbrella_edge(float side, float *x, float *y);  // screen position of the canopy's left (-1) / right (+1) tip
