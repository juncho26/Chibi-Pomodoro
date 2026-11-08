// Web simulator platform: the real UI/app code runs unchanged on top of this.
// Everything the board would supply (screen, touch, buttons, IMU, battery, clock, Wi-Fi, NVS, weather)
// is provided here, driven from JavaScript.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lvgl.h"
#include "app.h"
#include "ui.h"
#include "store.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "rtc.h"
#include "weather.h"
#include "imu.h"
#include "nvs.h"
#include "nvs_flash.h"

#define IMPORT(name) __attribute__((import_module("sim"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

IMPORT("now_us") double js_now_us(void);
IMPORT("random") uint32_t js_random(void);
IMPORT("nvs_get") int js_nvs_get(const char *key, void *buf, int cap);   // returns length or -1
IMPORT("nvs_set") void js_nvs_set(const char *key, const void *buf, int len);
IMPORT("event") void js_event(int kind, int arg);                       // 1 backlight, 2 sleep, 3 power off, 4 net mode

// ---------------------------------------------------------------- time
int64_t esp_timer_get_time(void) { return (int64_t)js_now_us(); }
uint32_t esp_random(void) { return js_random(); }
bool rtc_time_valid(void) { return true; }
void rtc_set_tz_offset_min(int m) { (void)m; }          // the JS clock already supplies local time
void rtc_set_epoch(time_t e) { (void)e; }

// ---------------------------------------------------------------- NVS (kept by the page, survives reboots)
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{
    (void)h;
    int n = js_nvs_get(k, v, v ? (int)*len : 0);
    if (n < 0) return ESP_ERR_NVS_NOT_FOUND;
    *len = (size_t)n; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { (void)h; js_nvs_set(k, v, (int)len); return ESP_OK; }

// ---------------------------------------------------------------- display
#define NW 172
#define NH 640
static lv_display_t *s_disp;
static uint8_t s_fb[NW * NH * 2];
static int s_backlight = 220;
static bool s_paused;
static orient_t s_disp_orient = ORI_L;
static int s_px;
static int s_sends;
static int32_t s_areas[64][4]; static int s_na;
EXPORT("sim_areas") int32_t *sim_areas(void) { return &s_areas[0][0]; }
EXPORT("sim_nareas") int sim_nareas(void) { int n = s_na; s_na = 0; return n; }
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)px; if (s_na < 64) { s_areas[s_na][0] = a->x1; s_areas[s_na][1] = a->y1; s_areas[s_na][2] = a->x2; s_areas[s_na][3] = a->y2; s_na++; } s_px += lv_area_get_size(a); if (lv_display_flush_is_last(d)) s_sends++; lv_display_flush_ready(d); }
EXPORT("sim_sends") int sim_sends(void) { int v = s_sends; s_sends = 0; return v; }
EXPORT("sim_px") int sim_px(void) { int v = s_px; s_px = 0; return v; }
static uint32_t tick_cb(void) { return (uint32_t)(js_now_us() / 1000); }

void display_set_orientation(orient_t o)
{
    // the sim draws the screen the way the viewer sees it: landscape 640x172 or portrait 172x640
    if (o < 0 || o > 3) return;
    s_disp_orient = o;
    bool land = o == ORI_L || o == ORI_LF;
    lv_display_set_resolution(s_disp, land ? NH : NW, land ? NW : NH);
    lv_display_set_buffers(s_disp, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_invalidate(lv_screen_active());
}
void display_backlight(int level) { s_backlight = level; js_event(1, level); }
void display_panel_sleep(bool sleep) { (void)sleep; }
void display_pause(bool p) { s_paused = p; }
void display_stats_tag(const char *n) { (void)n; }

// touch, in screen (logical) coordinates
static int s_tx, s_ty; static bool s_tdown;
bool display_touch_active(void) { return s_tdown; }
static void touch_read(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point.x = s_tx; d->point.y = s_ty;
    d->state = s_tdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---------------------------------------------------------------- buttons, battery
#define QN 16
static btn_event_t s_btn[QN]; static int s_bh, s_bt;
btn_event_t power_button_event(void) { if (s_bh == s_bt) return BTN_NONE; btn_event_t b = s_btn[s_bt]; s_bt = (s_bt + 1) % QN; return b; }
static int s_batt = 80; static bool s_usb;
int power_battery_pct(void) { return s_batt; }
bool power_on_usb(void) { return s_usb; }
bool power_battery_critical(void) { return !s_usb && s_batt <= 2; }
void power_off(void) { if (!s_usb) js_event(3, 0); }
uint8_t exio_read_inputs(void) { return 0; }
void esp_deep_sleep_start(void) { js_event(2, 0); }     // the page shows "asleep"; waking reboots

// ---------------------------------------------------------------- IMU
static orient_t s_orient = ORI_L;
static imu_event_t s_ev[QN]; static int s_eh, s_et;
static void push_ev(imu_ev_type_t t, orient_t o) { s_ev[s_eh] = (imu_event_t){ t, o }; s_eh = (s_eh + 1) % QN; }
bool imu_get_event(imu_event_t *ev) { if (s_eh == s_et) return false; *ev = s_ev[s_et]; s_et = (s_et + 1) % QN; return true; }
orient_t imu_current(void) { return s_orient; }
void imu_get_gravity(float g[3])
{
    static const float G[4][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
    memcpy(g, G[s_orient < 0 ? 0 : s_orient], sizeof(float) * 3);
}
void imu_set_calibration(const float ref[4][3], bool valid) { (void)ref; (void)valid; }
void imu_set_mapping(int m) { (void)m; }
void imu_arm_wake_on_motion(void) {}

// ---------------------------------------------------------------- Wi-Fi (pretend)
static net_mode_t s_net = NET_OFF; static int64_t s_net_t, s_net_act;
void net_start(void) { if (s_net == NET_OFF) { s_net = NET_STARTING; s_net_t = esp_timer_get_time(); js_event(4, s_net); } }
void net_stop(void) { if (s_net != NET_OFF) { s_net = NET_OFF; js_event(4, s_net); } }
net_mode_t net_mode(void)
{
    if (s_net == NET_STARTING && esp_timer_get_time() - s_net_t > 1500000) { s_net = NET_STA; s_net_act = esp_timer_get_time() / 1000; js_event(4, s_net); }
    return s_net;
}
int64_t net_last_activity_ms(void) { return s_net_act; }
int64_t net_last_user_ms(void) { return s_net_act; }
void powerlog_tick(int dt_ms, int screen, int net_mode, int light, int bright_pct) { (void)dt_ms; (void)screen; (void)net_mode; (void)light; (void)bright_pct; }
void powerlog_note_sleep(void) {}
void net_log_status(void) {}
bool net_quiet_finished(void) { return true; }
void net_qr_text(char *buf, int len) { snprintf(buf, len, "http://chibi.local/"); }

// ---------------------------------------------------------------- weather (chosen on the page)
static rain_info_t s_rain; static bool s_have_rain = true;
bool weather_rain_today(rain_info_t *out) { *out = s_rain; return s_have_rain; }

// ---------------------------------------------------------------- exports for the page
static lv_indev_t *s_ind;
EXPORT("sim_boot") void sim_boot(int orient)
{
    s_orient = orient < 0 ? ORI_L : (orient_t)orient;
    s_bh = s_bt = s_eh = s_et = 0; s_net = NET_OFF; s_tdown = false;
    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(NH, NW);
    lv_display_set_buffers(s_disp, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_timer_pause(lv_display_get_refr_timer(s_disp));
    s_ind = lv_indev_create();
    lv_indev_set_type(s_ind, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_ind, touch_read);
    lv_indev_set_long_press_time(s_ind, 1000);          // same as the device: "hold" needs a full second
    store_init();
    ui_init();
    app_init();
}
EXPORT("sim_frame") void sim_frame(void)
{
    ui_frame();
    lv_timer_pause(lv_display_get_refr_timer(s_disp));     // same as the device: only lv_refr_now draws
    lv_timer_handler();
    ui_render_prep();
    if (!s_paused) lv_refr_now(s_disp);
}
EXPORT("sim_fb") uint8_t *sim_fb(void) { return s_fb; }
EXPORT("sim_w") int sim_w(void) { return lv_display_get_horizontal_resolution(s_disp); }
EXPORT("sim_h") int sim_h(void) { return lv_display_get_vertical_resolution(s_disp); }
EXPORT("sim_stride") int sim_stride(void) { lv_draw_buf_t *b = lv_display_get_buf_active(s_disp); return b ? (int)b->header.stride : sim_w() * 2; }
EXPORT("sim_backlight") int sim_backlight(void) { return s_backlight; }
EXPORT("sim_screen_on") int sim_screen_on(void) { return !s_paused; }
EXPORT("sim_touch") void sim_touch(int x, int y, int down) { s_tx = x; s_ty = y; s_tdown = down; }
EXPORT("sim_button") void sim_button(int b) { s_btn[s_bh] = (btn_event_t)b; s_bh = (s_bh + 1) % QN; }
EXPORT("sim_orient") void sim_orient(int o) { s_orient = (orient_t)o; push_ev(IMU_EV_ORIENT, (orient_t)o); }
EXPORT("sim_knock") void sim_knock(void) { push_ev(IMU_EV_KNOCK3, ORI_NONE); }
EXPORT("sim_motion") void sim_motion(void) { push_ev(IMU_EV_MOTION, ORI_NONE); }
EXPORT("sim_battery") void sim_battery(int pct, int usb) { s_batt = pct; s_usb = usb; }
EXPORT("sim_weather") void sim_weather(int kind)
{
    // 0 none, 1 rain later today, 2 raining now, 3 snowing now
    s_have_rain = true;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    switch (kind) {
    case 1: s_rain = (rain_info_t){ .rain = true, .now = false, .from_hour = lt.tm_hour < 22 ? lt.tm_hour + 2 : 23, .max_prob = 40, .snow = false, .intensity = .35f }; break;
    case 2: s_rain = (rain_info_t){ .rain = true, .now = true, .from_hour = lt.tm_hour, .max_prob = 90, .snow = false, .intensity = .8f }; break;
    case 3: s_rain = (rain_info_t){ .rain = true, .now = true, .from_hour = lt.tm_hour, .max_prob = 80, .snow = true, .intensity = .6f }; break;
    default: s_rain = (rain_info_t){ 0 }; break;
    }
}
EXPORT("sim_net_activity") void sim_net_activity(void) { s_net_act = esp_timer_get_time() / 1000; }
// phone page stand-ins
static char s_str[128];
EXPORT("sim_strbuf") char *sim_strbuf(void) { return s_str; }
EXPORT("sim_task_add") void sim_task_add(void) { store_task_add(s_str); }
EXPORT("sim_task_toggle") void sim_task_toggle(int id) { bool d = false; store_lock(); tasklist_t *tl = store_tasks(); for (int i = 0; i < tl->n; i++) if (tl->t[i].id == id) d = tl->t[i].done; store_unlock(); store_task_set_done(id, !d); }
EXPORT("sim_task_count") int sim_task_count(void) { return store_tasks()->n; }
EXPORT("sim_task_id") int sim_task_id(int i) { return store_tasks()->t[i].id; }
EXPORT("sim_task_done") int sim_task_done(int i) { return store_tasks()->t[i].done; }
EXPORT("sim_task_name") const char *sim_task_name(int i) { return store_tasks()->t[i].name; }
EXPORT("sim_screen") int sim_screen(void) { return app_screen(); }
// fast-forward the running timer
EXPORT("sim_skip") void sim_skip(int ms)
{
    store_lock(); session_t *ss = store_session();
    if (ss->study_running && ss->study_left_ms > 0) { ss->study_left_ms = ss->study_left_ms > ms + 1500 ? ss->study_left_ms - ms : 1500; ss->studied_ms += ms; }
    else if (ss->break_active && ss->break_left_ms > 0) ss->break_left_ms = ss->break_left_ms > ms + 1500 ? ss->break_left_ms - ms : 1500;
    store_unlock();
}
EXPORT("sim_session_info") int sim_session_info(int what)
{
    session_t *ss = store_session();
    switch (what) { case 0: return ss->study_left_ms; case 1: return ss->break_active ? ss->break_left_ms : -1; case 2: return ss->carry; default: return ss->sessions; }
}
EXPORT("sim_task_delete") void sim_task_delete(int id) { store_task_delete((uint16_t)id); }
static int count_objs(lv_obj_t *o) { int n = 1; for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n += count_objs(lv_obj_get_child(o, i)); return n; }
EXPORT("sim_objs") int sim_objs(void) { return count_objs(lv_screen_active()) + count_objs(lv_layer_top()); }
EXPORT("sim_reset_store") void sim_reset_store(void) {}
// test page: every icon, large, in a grid (for checking shapes)
EXPORT("sim_icon_sheet") void sim_icon_sheet(int first, int size)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_display_set_resolution(s_disp, NH, NW);
    lv_display_set_buffers(s_disp, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    int per = NH / (size + 8);
    for (int i = 0; i < per && first + i < IC_COUNT; i++) icon_create(scr, (icon_t)(first + i), 4 + size / 2 + i * (size + 8) + size / 6, NW / 2, size, 0xFFFFFF);
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(s_disp);
}
// test hook: repaint everything (to compare against the partial repaint)
EXPORT("sim_full_redraw") void sim_full_redraw(void) { lv_obj_invalidate(lv_screen_active()); lv_obj_invalidate(lv_layer_top()); lv_refr_now(s_disp); }
#include "character.h"
static int32_t s_dbg_box[5];
EXPORT("sim_task_font") void sim_task_font(int lvl) { store_lock(); store_settings()->task_font = lvl; store_touch(); store_unlock(); }
EXPORT("sim_ch_box") int32_t *sim_ch_box(void) { lv_area_t a; uint32_t h; s_dbg_box[4] = ch_measure(&a, &h); s_dbg_box[0] = a.x1; s_dbg_box[1] = a.y1; s_dbg_box[2] = a.x2; s_dbg_box[3] = a.y2; return s_dbg_box; }

/* ---- text-size page support: the device's own glyph widths and "..." cut, for the web preview ---- */
extern const lv_font_t font_jp13, font_jp16, font_jp19, font_jp22;
static const lv_font_t *const s_tfonts[4] = { &font_jp13, &font_jp16, &font_jp19, &font_jp22 };
EXPORT("sim_glyph_w") int sim_glyph_w(int lvl, uint32_t letter, uint32_t next)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(s_tfonts[lvl], &g, letter, next)) return -1;   // not in the font
    return lv_font_get_glyph_width(s_tfonts[lvl], letter, next);
}
static char s_cut[256];
EXPORT("sim_label_cut") const char *sim_label_cut(int lvl, const char *txt)
{
    static lv_obj_t *l;
    if (!l) { l = lv_label_create(lv_layer_top()); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
    const lv_font_t *f = s_tfonts[lvl];
    int cs = 12 + lvl * 2, lx = 9 + cs + 8;            // same as row_fill in ui.c
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_size(l, 152 - lx - 5, f->line_height);
    lv_label_set_text(l, txt);
    lv_obj_update_layout(l);
    lv_strlcpy(s_cut, lv_label_get_text(l), sizeof s_cut);
    return s_cut;
}
#include "textfit.h"
EXPORT("sim_textfit") int sim_textfit(int lvl, const char *txt) { return textfit_keep(lvl, txt); }
