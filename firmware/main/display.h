#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "imu.h"

lv_display_t *display_init(void);          // LCD + touch + LVGL. Backlight stays off until display_backlight().
void display_set_orientation(orient_t o);   // L=270, LF=90, P=0, PF=180 (character always on the same physical end)
void display_backlight(int level);          // 0..255, 0 = off
void display_panel_sleep(bool sleep);
uint32_t display_flush_count(void);      // frames drawn so far (to notice a still screen)
bool display_touch_active(void);            // finger currently on the screen
void display_pause(bool pause);             // screen off: stop drawing and sending frames
void display_stats_tag(const char *name);   // which screen the frame statistics belong to
int  display_stats_json(char *buf, int len);  // last 5-second window: fps, slowest frame, time per stage
