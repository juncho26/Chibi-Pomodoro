#pragma once
#include <stdbool.h>
#include "imu.h"

typedef enum { SCR_HOME, SCR_STUDY, SCR_DONE, SCR_BREAK, SCR_TASKS, SCR_CLEAR, SCR_CARRY, SCR_SETTINGS, SCR_CAL } screen_t;

void app_init(void);
void app_tick(void);                 // called by the UI loop every frame
screen_t app_screen(void);
orient_t app_orient(void);
bool app_low_battery(void);
int  app_settings_page(void);        // timer screen: 0 study, 1 break · device screen (swipe left): 2 brightness, 3 direction
// Direction calibration (Settings > Direction)
void app_cal_start(void);
void app_cal_cancel(void);
void app_cal_reset(void);            // back to the built-in directions
bool app_cal_valid(void);
int  app_cal_step(void);             // 1..4 while calibrating (Home, Study, Tasks, Clear), 0 otherwise
float app_cal_progress(void);        // 0..1 how long the device has been held still in this step
// Brightness (Settings > Brightness), 10..100 %
int  app_brightness_pct(void);
void app_set_brightness_pct(int pct);
void app_settings_set_page(int p);
void app_settings_step(int dir);     // -1 / +1
void app_done_skip(void);
void app_study_reset(void);
void app_study_toggle_pause(void);  // single tap in Study
bool app_end_session(void);         // hold on Break (or Home with a paused session): end it, no extension         // double-tap in Study: restart the countdown
void app_activity(void);
bool app_dimmed(void);              // screen dimmed after 90 s idle (the picture drops to 15 fps)
float app_now_ms(void);
