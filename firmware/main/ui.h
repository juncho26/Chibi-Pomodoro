#pragma once
#include "app.h"
#include "lvgl.h"
#include "icons.h"

void ui_init(void);
void ui_frame(void);
void ui_render_prep(void);                        // after lv_timer_handler, before painting                              // one frame of logic + animation (called at 60 Hz by the UI task)
void ui_show(screen_t s, orient_t o, float turn_deg);   // applied at the start of the next frame
void ui_show_now(screen_t s, orient_t o);                 // immediate (boot)
void ui_settings_changed(int dir);
void ui_cal_captured(void);
void ui_settings_swipe(bool left);   // Settings: left = device screen, right = timer screen         // direction calibration: a position was recorded
void ui_knock(void);
bool ui_digits_busy(void);                         // timer digits still animating in
void ui_study_reset_fx(void);
void ui_study_pause_fx(bool paused);
void ui_toast(icon_t a, icon_t b);                  // short message as one or two icons
void ui_message(const char *txt);
void ui_message_hide_after(int ms);
