#pragma once
#include "lvgl.h"
// How a task name fits on a device row at text size `lvl` (0-3).
// Returns -1 when it fits, else how many characters stay before the "..." -
// worked out exactly as the row's LVGL label does it, with the device's own fonts.
// Uses only the read-only font data, so any task may call it.
int textfit_keep(int lvl, const char *txt);
extern const lv_font_t *const TEXTFIT_FONTS[4];
static inline int textfit_width(int lvl) { int cs = 12 + lvl * 2; return 152 - (9 + cs + 8) - 5; }   // = row_fill in ui.c
