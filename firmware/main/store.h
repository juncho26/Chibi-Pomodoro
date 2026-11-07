#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_TASKS 20
#define TASK_NAME_MAX 124          // 40 characters of UTF-8 (Japanese is 3 bytes each)

typedef struct { uint16_t id; uint8_t done; char name[TASK_NAME_MAX]; } task_t;
typedef struct { task_t t[MAX_TASKS]; uint8_t n; uint16_t next_id; } tasklist_t;

typedef struct {
    uint8_t study_min, break_min, imu_map, brightness;
    int16_t tz_min;                // minutes east of UTC (Japan = 540)
    uint8_t task_font;             // size of the task list text on the device: 0 small .. 3 extra large
} settings_t;

typedef struct {
    int32_t study_left_ms;         // -1 = no session in progress
    int32_t break_left_ms;
    uint8_t study_running, done_await, skip_break, break_active, carry;
    uint16_t sessions;
    int16_t sessions_yday;         // day-of-year the count belongs to
    int32_t break_total_ms;        // length of the current break (for its progress bar)
    int32_t studied_ms;            // study time since the last break (an early stop needs 1 min of it)
    uint8_t study_paused;          // paused by a tap on the Study screen
} session_t;

typedef struct { char ssid[33]; char pass[65]; char ap_pass[12]; char token[12]; } netcfg_t;

// Weather: a place name typed on the phone, resolved to coordinates by the device once it's online.
// on = weather wanted; autoloc = estimate the place from the internet connection instead of `city`.
typedef struct { char city[64]; char place[64]; float lat, lon; uint8_t located, on, autoloc; } wxcfg_t;

// Direction calibration: gravity measured in each of the four positions (Home, Study, Tasks, Clear).
typedef struct { float ref[4][3]; uint8_t valid; } imucal_t;

// Saved Wi-Fi networks (home, phone hotspot, school...). The device joins whichever is in range.
#define MAX_WIFI 3
typedef struct { char ssid[33]; char pass[65]; } wifi_t;
typedef struct { wifi_t w[MAX_WIFI]; uint8_t n; } wifilist_t;

void store_init(void);
void store_lock(void);
void store_unlock(void);

// All of these must be accessed between store_lock()/store_unlock().
tasklist_t *store_tasks(void);
settings_t *store_settings(void);
session_t  *store_session(void);
netcfg_t   *store_net(void);
wxcfg_t    *store_wx(void);
wifilist_t *store_wifis(void);
imucal_t   *store_cal(void);
uint32_t    store_version(void);      // bumps on every change, so the UI knows to refresh

void store_mark_tasks(void);          // call after changing tasks (inside the lock)
void store_mark_settings(void);
void store_touch(void);               // tell the UI something changed, without saving (live previews)
void store_mark_session(void);
void store_mark_net(void);
void store_mark_wx(void);
void store_mark_wifis(void);
void store_mark_cal(void);
void store_flush(void);               // writes dirty parts to flash (call from the UI loop, outside the lock)

// helpers (take the lock themselves)
bool store_task_add(const char *name);
bool store_task_set_done(uint16_t id, bool done);
bool store_task_rename(uint16_t id, const char *name);
bool store_task_delete(uint16_t id);
int  store_task_clear_done(void);
void store_task_reorder(const uint16_t *ids, int n);
int  store_wifi_set(const char *ssid, const char *pass);   // add or update; 0 ok, -1 list full
bool store_wifi_remove(const char *ssid);
