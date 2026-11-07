#pragma once
#include "lvgl.h"

// Small vector icons drawn in the same flat line style as the character, so the tiny screen needs
// almost no words. Coordinates are designed in a unit box and scaled to any size.
typedef enum {
    IC_NONE,
    IC_FLIP,        // two arrows turning: flip the device
    IC_BOOK,        // study
    IC_CUP,         // break
    IC_PAUSE, IC_PLAY, IC_STOP,
    IC_TAP,         // dot with rays: tap
    IC_HOLD,        // dot in a ring: press and hold
    IC_CHECK,       // tick
    IC_DONE,        // tick in a circle: finished sessions
    IC_TASKS,       // checklist
    IC_SWEEP,       // checklist with a slash: clear checked tasks
    IC_PHONE, IC_WIFI, IC_LOCK, IC_SUN,
    IC_ROTATE,      // device with a turning arrow: direction
    IC_RESET,       // circular arrow back
    IC_CROSS, IC_UMBRELLA, IC_SNOW, IC_PLUG, IC_POWER, IC_BATT_LOW, IC_HOME,
    IC_CHEV_L, IC_CHEV_R,
    IC_SKIP,        // >> skip
    IC_UP,          // arrow up: this way up
    IC_STAR,
    IC_KNOCK,       // ●● ●
    IC_PLUS, IC_MINUS,
    IC_DONE_FLIP,   // tick inside two turning arrows: finished, flip to go on
    IC_COUNT
} icon_t;

void icon_draw(lv_layer_t *layer, icon_t id, float cx, float cy, float size, lv_color_t col, lv_opa_t opa);

// An LVGL object that draws one icon centred in a size x size box (cx, cy = centre).
lv_obj_t *icon_create(lv_obj_t *parent, icon_t id, int cx, int cy, int size, uint32_t color);
void icon_set(lv_obj_t *o, icon_t id);
void icon_set_color(lv_obj_t *o, uint32_t color);
