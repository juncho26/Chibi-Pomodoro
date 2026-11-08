// Mode rules (same as the simulator):
//  Landscape = Home, flipped landscape = Study, portrait = Tasks, flipped portrait = Clear tasks.
//  Leaving Study mid-session pauses it; going to Home also starts a break. Study end -> celebrate,
//  then flip for a break (or tap / BOOT to skip it). Knock ●● ● = carry mode: everything freezes and
//  orientation is ignored until the next knock. Hold BOOT = settings (timers frozen while open).
#include "app.h"
#include "powerlog.h"
#include "weather.h"
#include "ui.h"
#include "store.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "rtc.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "board.h"

static const char *TAG = "app";
static const int STAGES_STUDY[] = {15,20,25,30,35,40,45,50,60};
static const int STAGES_BREAK[] = {3,5,10,15,20};

static orient_t s_orient = ORI_L;
static bool s_settings;
static int s_page;
static int s_cal_step;                 // direction calibration: 0 = off, 1..4
static screen_t s_screen = SCR_HOME;
static int64_t s_last_ms, s_last_act, s_last_save, s_settings_act;
static bool s_dim, s_screen_off;
#define DIM_LEVEL 26                    // 10% of full brightness
static int s_bright = 220;

float app_now_ms(void) { return (float)(esp_timer_get_time() / 1000); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
screen_t app_screen(void) { return s_screen; }
orient_t app_orient(void) { return s_orient; }
int app_settings_page(void) { return s_page; }

bool app_low_battery(void)
{
    int p = power_battery_pct();
    return p >= 0 && p <= 15 && !power_on_usb();
}

void app_activity(void)
{
    s_last_act = now_ms();
    if (s_dim || s_screen_off) { s_dim = false; s_screen_off = false; display_pause(false); display_backlight(s_bright); }
}

bool app_dimmed(void) { return s_dim; }

static screen_t compute_screen(void)
{
    session_t *ss = store_session();
    if (ss->carry) return SCR_CARRY;
    if (s_settings) return s_cal_step ? SCR_CAL : SCR_SETTINGS;
    switch (s_orient) {
    case ORI_L:  return ss->break_active ? SCR_BREAK : SCR_HOME;
    case ORI_LF: return ss->done_await ? SCR_DONE : SCR_STUDY;
    case ORI_P:  return SCR_TASKS;
    default:     return SCR_CLEAR;
    }
}

static void refresh(float turn_deg)
{
    screen_t k = compute_screen();
    if (k != s_screen || turn_deg != 0) {
        s_screen = k;
        if (k == SCR_TASKS) net_start();
        ui_show(k, s_orient, turn_deg);
    }
}

static float turn_angle(orient_t from, orient_t to)
{
    static const int ANG[4] = { 180, 0, 270, 90 };
    int th = ((ANG[to] - ANG[from] + 540) % 360) - 180;
    if (abs(th) == 180) th = (rand() & 1) ? 180 : -180;
    return (float)th;
}

static void start_break(int32_t ms) { session_t *ss = store_session(); ss->break_active = 1; ss->break_left_ms = ms; ss->break_total_ms = ms; }
#define EARLY_BREAK_MIN_STUDY_MS 60000

static void set_orient(orient_t o)
{
    if (o == ORI_NONE || o == s_orient) return;
    store_lock();
    session_t *ss = store_session();
    orient_t prev = s_orient;
    s_orient = o;
    if (prev == ORI_LF) {
        if (ss->done_await) {
            // finished session: full break
            ss->done_await = 0; ss->study_left_ms = -1; ss->studied_ms = 0;
            if (o == ORI_L && !ss->skip_break) start_break(store_settings()->break_min * 60000);
        } else if (ss->study_left_ms > 0) {
            // stopped early (running or paused): half the break, every time, as long as you studied
            // at least a minute since the last break. Quick flip-flops just pause, so breaks can't chain.
            ss->study_running = 0; ss->study_paused = 0;
            if (o == ORI_L) {
                if (ss->studied_ms >= EARLY_BREAK_MIN_STUDY_MS) { start_break(store_settings()->break_min * 30000); ss->studied_ms = 0; }
                // under a minute: no break, just Home with the timer paused (shown bottom right)
            }
        }
    }
    if (o == ORI_LF) {
        ss->break_active = 0; ss->break_left_ms = 0;
        if (!ss->done_await) {
            if (ss->study_left_ms < 0) { ss->study_left_ms = store_settings()->study_min * 60000; ss->studied_ms = 0; }
            ss->study_running = 1; ss->study_paused = 0;
        }
    }
    store_mark_session();
    store_unlock();
    refresh(turn_angle(prev, o));
}

// Ends the whole session from Study, Break, or Home with a paused study. It never adds time, so breaks
// still can't be stretched. On Study the screen stays put, ready for a fresh session (tap to start).
bool app_end_session(void)
{
    store_lock();
    session_t *ss = store_session();
    bool any = ss->break_active || ss->study_left_ms > 0;
    bool on_study = s_screen == SCR_STUDY;
    if (any && (on_study || s_screen == SCR_BREAK || (s_screen == SCR_HOME && !ss->study_running))) {
        ss->break_active = 0; ss->break_left_ms = 0;
        ss->study_left_ms = -1; ss->study_running = 0; ss->done_await = 0; ss->skip_break = 0;
        ss->studied_ms = 0; ss->study_paused = on_study;
        store_mark_session();
    } else any = false;
    store_unlock();
    if (!any) return false;
    refresh(0);
    ui_show(on_study ? SCR_STUDY : SCR_HOME, s_orient, 0);   // the paused timer vanishing says it ended
    return true;
}

// Single tap on Study: pause or resume (or start, after a session was ended there).
void app_study_toggle_pause(void)
{
    if (s_screen != SCR_STUDY) return;
    store_lock();
    session_t *ss = store_session();
    bool now_paused;
    if (ss->study_running) { ss->study_running = 0; ss->study_paused = 1; now_paused = true; }
    else {
        if (ss->study_left_ms < 0) { ss->study_left_ms = store_settings()->study_min * 60000; ss->studied_ms = 0; }
        ss->study_running = 1; ss->study_paused = 0; now_paused = false;
    }
    store_mark_session();
    store_unlock();
    ui_study_pause_fx(now_paused);
}

void app_done_skip(void)
{
    store_lock(); store_session()->skip_break = 1; store_mark_session(); store_unlock();
    ui_show(s_screen, s_orient, 0);
}

// Double-tap in Study restarts the session timer. Breaks can't be extended this way (no handler there).
void app_study_reset(void)
{
    if (s_screen != SCR_STUDY) return;
    store_lock();
    session_t *ss = store_session();
    ss->study_left_ms = store_settings()->study_min * 60000;
    ss->study_running = 1; ss->study_paused = 0;
    store_mark_session();
    store_unlock();
    ui_study_reset_fx();
}

void app_settings_set_page(int p) { s_page = (p + 4) % 4; s_settings_act = now_ms(); ui_show(SCR_SETTINGS, s_orient, 0); }

// ---------------------------------------------------------------- brightness
int app_brightness_pct(void) { return (s_bright * 100 + 127) / 255; }
void app_set_brightness_pct(int pct)
{
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    s_bright = (pct * 255 + 50) / 100;
    s_settings_act = now_ms();
    if (!s_dim && !s_screen_off) display_backlight(s_bright);
    store_lock(); store_settings()->brightness = s_bright; store_mark_settings(); store_unlock();
}

// ---------------------------------------------------------------- direction calibration
// Each step turns the screen to how that mode is drawn and asks you to turn the device until the
// text reads upright. Holding still for 1.2 s records gravity for that position.
static float s_cal_ref[4][3], s_cal_avg[3], s_cal_prog;
static int64_t s_cal_still_since, s_cal_next_at;
static const orient_t CAL_POSE[4] = { ORI_L, ORI_LF, ORI_P, ORI_PF };

bool app_cal_valid(void) { bool v; store_lock(); v = store_cal()->valid; store_unlock(); return v; }
int app_cal_step(void) { return s_cal_step; }
float app_cal_progress(void) { return s_cal_prog; }

void app_cal_start(void)
{
    s_cal_step = 1; s_cal_prog = 0; s_cal_still_since = 0; s_cal_next_at = 0;
    s_settings_act = now_ms();
    s_screen = SCR_CAL;
    ui_show(SCR_CAL, CAL_POSE[0], 0);
}

void app_cal_cancel(void)
{
    if (!s_cal_step) return;
    s_cal_step = 0;
    s_screen = SCR_SETTINGS;
    ui_show(SCR_SETTINGS, s_orient, 0);
}

void app_cal_reset(void)
{
    store_lock(); store_cal()->valid = 0; store_mark_cal(); store_unlock();
    imu_set_calibration(s_cal_ref, false);
    ui_show(SCR_SETTINGS, s_orient, 0);
    ui_toast(IC_RESET, IC_NONE); ui_message_hide_after(1600);
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *v) { float n = sqrtf(dot3(v, v)); if (n > 0.01f) for (int i = 0; i < 3; i++) v[i] /= n; }

static void cal_tick(int64_t now)
{
    s_settings_act = now;                          // don't auto-close while calibrating
    if (s_cal_next_at) {                           // short pause after a capture, then the next step
        if (now < s_cal_next_at) return;
        s_cal_next_at = 0;
        if (s_cal_step > 4) {
            // Check the four positions make sense: each opposite pair points opposite ways and
            // landscape is apart from portrait. A device leaning back on a stand adds the same tilt
            // to all four, so that shared part is taken out first (it used to fail the check and
            // silently start over at 1/4).
            float lean[3] = { 0, 0, 0 }, r[4][3];
            for (int k = 0; k < 4; k++) for (int i = 0; i < 3; i++) lean[i] += s_cal_ref[k][i] / 4;
            bool tilted = dot3(lean, lean) > 0.04f; norm3(lean);
            for (int k = 0; k < 4; k++) {
                float d = tilted ? dot3(s_cal_ref[k], lean) : 0;
                for (int i = 0; i < 3; i++) r[k][i] = s_cal_ref[k][i] - d * lean[i];
                norm3(r[k]);
            }
            bool ok = dot3(r[0], r[1]) < -0.5f && dot3(r[2], r[3]) < -0.5f && fabsf(dot3(r[0], r[2])) < 0.6f;
            ESP_LOGI("cal", "check: L.LF %.2f  P.PF %.2f  L.P %.2f  lean %s -> %s", dot3(r[0], r[1]), dot3(r[2], r[3]),
                     dot3(r[0], r[2]), tilted ? "yes" : "no", ok ? "saved" : "rejected");
            if (!ok) {
                s_cal_step = 1; s_cal_prog = 0; s_cal_still_since = 0;
                ui_show(SCR_CAL, CAL_POSE[0], 0);
                ui_toast(IC_CROSS, IC_RESET); ui_message_hide_after(2400);
                return;
            }
            store_lock(); imucal_t *c = store_cal(); memcpy(c->ref, s_cal_ref, sizeof s_cal_ref); c->valid = 1; store_mark_cal(); store_unlock();
            imu_set_calibration(s_cal_ref, true);
            s_cal_step = 0; s_screen = SCR_SETTINGS;
            store_flush();
            ui_show(SCR_SETTINGS, s_orient, 0);
            ui_toast(IC_CHECK, IC_NONE); ui_message_hide_after(1800);
            return;
        }
        s_cal_prog = 0; s_cal_still_since = 0;
        ui_show(SCR_CAL, CAL_POSE[s_cal_step - 1], 0);
        return;
    }
    float g[3]; imu_get_gravity(g);
    float mag = sqrtf(dot3(g, g));
    bool upright_ish = mag > 0.8f && mag < 1.2f;
    // still = gravity hasn't moved more than ~3 degrees from its running average
    float d[3] = { g[0] - s_cal_avg[0], g[1] - s_cal_avg[1], g[2] - s_cal_avg[2] };
    bool still = upright_ish && sqrtf(dot3(d, d)) < 0.05f;
    for (int i = 0; i < 3; i++) s_cal_avg[i] += (g[i] - s_cal_avg[i]) * 0.2f;
    // must differ from the positions already recorded (someone forgot to turn it)
    float u[3] = { g[0], g[1], g[2] }; norm3(u);
    for (int k = 0; k < s_cal_step - 1; k++) if (dot3(u, s_cal_ref[k]) > 0.7f) still = false;
    if (!still) { s_cal_still_since = 0; s_cal_prog = 0; return; }
    if (!s_cal_still_since) s_cal_still_since = now;
    s_cal_prog = (now - s_cal_still_since) / 1200.f;
    if (s_cal_prog >= 1) {
        s_cal_prog = 1;
        memcpy(s_cal_ref[s_cal_step - 1], u, sizeof u);
        ui_cal_captured();
        s_cal_step++;
        s_cal_next_at = now + 700;
    }
}

void app_settings_step(int dir)
{
    s_settings_act = now_ms();
    if (s_page == 2) { app_set_brightness_pct(((app_brightness_pct() + 5) / 10) * 10 + dir * 10); ui_settings_changed(dir); return; }
    if (s_page == 3) return;
    store_lock();
    settings_t *st = store_settings();
    {
        const int *l = s_page ? STAGES_BREAK : STAGES_STUDY; int n = s_page ? 5 : 9;
        uint8_t *v = s_page ? &st->break_min : &st->study_min;
        int i = 0; while (i < n - 1 && l[i] != *v) i++;
        int ni = i + dir; if (ni < 0) ni = 0; if (ni >= n) ni = n - 1;
        *v = l[ni];
    }
    store_mark_settings();
    store_unlock();
    ui_settings_changed(dir);
}

// Timers are paused while Settings is open. On the way out, a running (or paused) study or break
// keeps the time already spent: new remaining = new length - time already counted down.
// If that's nothing left, the timer finishes on the next tick as usual.
static uint8_t s_set_study0, s_set_break0;
static void settings_apply_lengths(void)
{
    store_lock();
    session_t *ss = store_session(); settings_t *st = store_settings();
    if (ss->study_left_ms > 0 && st->study_min != s_set_study0) {
        int32_t elapsed = s_set_study0 * 60000 - ss->study_left_ms;
        if (elapsed < 0) elapsed = 0;
        int32_t left = st->study_min * 60000 - elapsed;
        ss->study_left_ms = left > 0 ? left : 1;
        store_mark_session();
    }
    if (ss->break_active && st->break_min != s_set_break0) {
        int32_t total0 = ss->break_total_ms > 0 ? ss->break_total_ms : s_set_break0 * 60000;
        int32_t elapsed = total0 - ss->break_left_ms;
        if (elapsed < 0) elapsed = 0;
        // a half-length (early-stop) break stays half-length
        int32_t total = (int32_t)((int64_t)st->break_min * 60000 * total0 / (s_set_break0 * 60000));
        int32_t left = total - elapsed;
        ss->break_total_ms = total;
        ss->break_left_ms = left > 0 ? left : 1;
        store_mark_session();
    }
    store_unlock();
}

static void toggle_settings(void)
{
    s_settings = !s_settings;
    s_settings_act = now_ms();
    if (s_settings) {
        s_page = 0;
        store_lock(); s_set_study0 = store_settings()->study_min; s_set_break0 = store_settings()->break_min; store_unlock();
    } else {
        s_cal_step = 0;
        settings_apply_lengths(); store_flush();
        refresh(0);
        orient_t phys = imu_current();          // you may have turned it while Settings was open
        if (phys != ORI_NONE && phys != s_orient) set_orient(phys);
        return;
    }
    refresh(0);
}

static void toggle_carry(void)
{
    store_lock();
    session_t *ss = store_session();
    ss->carry = !ss->carry;
    store_mark_session();
    bool carry = ss->carry;
    store_unlock();
    ui_knock();
    if (carry) { if (s_settings) { s_settings = false; s_cal_step = 0; settings_apply_lengths(); } refresh(0); }
    else {
        refresh(0);
        orient_t phys = imu_current();
        if (phys != ORI_NONE && phys != s_orient) set_orient(phys);
    }
}

static void deep_sleep_now(void)
{
    ESP_LOGW(TAG, "going to deep sleep");
    powerlog_note_sleep();
    store_flush();
    net_stop();
    display_backlight(0);
    display_panel_sleep(true);
    imu_arm_wake_on_motion();
    vTaskDelay(pdMS_TO_TICKS(50));
    exio_read_inputs();                       // clears a pending TCA9554 INT
    uint64_t mask = 1ULL << PIN_PWR_BTN;
#if CONFIG_CHIBI_BOARD_V2
    mask |= 1ULL << PIN_EXIO_INT;             // motion (IMU INT1 -> EXIO2) or touch via the expander
    rtc_gpio_pullup_en(PIN_EXIO_INT); rtc_gpio_pulldown_dis(PIN_EXIO_INT);
#endif
    rtc_gpio_pullup_en(PIN_PWR_BTN); rtc_gpio_pulldown_dis(PIN_PWR_BTN);
    esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

static void shutdown(icon_t why)
{
    ui_toast(IC_POWER, why);
    lv_refr_now(NULL);
    store_flush();
    vTaskDelay(pdMS_TO_TICKS(600));
    power_off();                              // returns only on USB power
    ui_message_hide_after(800);             // on USB power it can't turn off: just carry on
}


void app_init(void)
{
    store_lock();
    settings_t *st = store_settings();
    imu_set_mapping(st->imu_map);
    if (store_cal()->valid) imu_set_calibration(store_cal()->ref, true);
    rtc_set_tz_offset_min(st->tz_min);
    s_bright = st->brightness ? st->brightness : 220;
    session_t *ss = store_session();
    if (ss->study_running) ss->study_running = 0;   // resumes when held in study position
    store_unlock();
    // wait briefly for a stable orientation
    for (int i = 0; i < 60 && imu_current() == ORI_NONE; i++) vTaskDelay(pdMS_TO_TICKS(10));
    orient_t o = imu_current();
    s_orient = o == ORI_NONE ? ORI_L : o;
    store_lock();
    if (s_orient == ORI_LF && !ss->done_await && !ss->carry) {
        if (!ss->study_paused) {
            if (ss->study_left_ms < 0) { ss->study_left_ms = store_settings()->study_min * 60000; ss->studied_ms = 0; }
            ss->study_running = 1;
        }
    }
    store_unlock();
    s_last_ms = s_last_act = s_last_save = now_ms();
    s_screen = compute_screen();
    if (s_screen == SCR_TASKS) net_start();
    ui_show_now(s_screen, s_orient);          // draw the first frame before the backlight comes on
    display_backlight(s_bright);
}

void app_tick(void)
{
    int64_t now = now_ms();
    int dt = (int)(now - s_last_ms);
    s_last_ms = now;
    if (dt > 1000) dt = 1000;

    // ---- inputs ----
    imu_event_t ev;
    while (imu_get_event(&ev)) {
        if (ev.type == IMU_EV_MOTION) { if (!s_screen_off) app_activity(); continue; }
        app_activity();
        bool carry; store_lock(); carry = store_session()->carry; store_unlock();
        if (ev.type == IMU_EV_KNOCK3) toggle_carry();             // from Settings too: it saves and closes them first
        else if (ev.type == IMU_EV_ORIENT && !carry && !s_settings) set_orient(ev.orient);
    }
    if (display_touch_active()) app_activity();
    btn_event_t b;
    while ((b = power_button_event()) != BTN_NONE) {
        bool was_off = s_screen_off;
        app_activity();
        bool carry; store_lock(); carry = store_session()->carry; store_unlock();
        switch (b) {
        case BTN_BOOT_LONG: if (!carry) toggle_settings(); else toggle_carry(); break;
        case BTN_BOOT_SHORT:
            if (carry) toggle_carry();                // a press of BOOT always gets you out of carry mode
            else if (s_settings && s_cal_step) app_cal_cancel();
            else if (s_settings) app_settings_set_page(s_page + 1);
            else if (s_screen == SCR_DONE) app_done_skip();
            break;
        case BTN_PWR_SHORT:
            if (!was_off) {
                bool busy; store_lock(); busy = store_session()->study_running || store_session()->break_active; store_unlock();
                if (busy || net_mode() != NET_OFF) { s_screen_off = true; display_backlight(0); display_pause(true); }
                else deep_sleep_now();
            }
            break;
        case BTN_PWR_LONG: shutdown(IC_NONE); break;
        default: break;
        }
    }
    if (net_mode() != NET_OFF && now - net_last_user_ms() < 2000) app_activity();

    // ---- timers (frozen in carry mode and while settings are open) ----
    store_lock();
    session_t *ss = store_session();
    bool running = false;
    // the countdown starts once its digits have landed, so no second is lost behind the animation
    bool settling = (s_screen == SCR_STUDY || s_screen == SCR_BREAK) && ui_digits_busy();
    if (!ss->carry && !s_settings && !settling) {
        if (ss->study_running) {
            running = true;
            ss->study_left_ms -= dt;
            if (ss->studied_ms < 0x40000000) ss->studied_ms += dt;
            if (ss->study_left_ms <= 0) {
                ss->study_left_ms = -1; ss->study_running = 0; ss->done_await = 1; ss->skip_break = 0; ss->sessions++;
                store_mark_session();
                store_unlock(); refresh(0); store_lock();
            }
        }
        if (ss->break_active) {
            running = true;
            ss->break_left_ms -= dt;
            if (ss->break_left_ms <= 0) { ss->break_active = 0; ss->break_left_ms = 0; store_mark_session(); store_unlock(); refresh(0); store_lock(); }
        }
    }
    // sessions counter resets at local midnight
    if (rtc_time_valid()) {
        time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
        if (ss->sessions_yday != lt.tm_yday) { if (ss->sessions_yday >= 0) ss->sessions = 0; ss->sessions_yday = lt.tm_yday; store_mark_session(); }
    }
    bool carry = ss->carry, idle_ok = !ss->study_running && !ss->break_active;
    if (running && now - s_last_save > 120000) { store_mark_session(); s_last_save = now; }   // flash writes pause the whole chip for a moment: every 2 min is enough to survive a power cut
    store_unlock();

    if (s_cal_step) cal_tick(now);

    // ---- settings auto-close after 20 s ----
    if (s_settings && now - s_settings_act > 20000) toggle_settings();

    // ---- network: off 5 minutes after the last request once you leave the Tasks screen ----
    // Wi-Fi is the biggest drain after the screen. Even on Tasks it goes off after 10 minutes with
    // nobody using the page or touching the device; touching the Tasks screen turns it back on.
    if (net_mode() == NET_STA || net_mode() == NET_AP) {
        if (s_screen != SCR_TASKS && (net_quiet_finished() || now - net_last_activity_ms() > 5 * 60000)) net_stop();
        else if (s_screen == SCR_TASKS && now - net_last_activity_ms() > 10 * 60000 && now - s_last_act > 10 * 60000) net_stop();
    } else if (s_screen == SCR_TASKS && net_mode() == NET_OFF && display_touch_active()) net_start();
    { static int64_t last_status; if (net_mode() != NET_OFF && now - last_status > 60000) { last_status = now; net_log_status(); } }

    // ---- power ----
    if (power_battery_critical()) shutdown(IC_BATT_LOW);
    int64_t idle = now - s_last_act;
    // Left alone, the screen only dims (to 10%) and keeps showing everything. It turns off only in
    // carry mode, where nobody is looking.
    if (!s_dim && !s_screen_off && idle > 90000) { s_dim = true; display_backlight(s_bright < DIM_LEVEL ? s_bright : DIM_LEVEL); }
    bool net_idle = net_mode() == NET_OFF;
    if (carry && idle > 60000 && net_idle) deep_sleep_now();
    (void)idle_ok;
    powerlog_tick(dt, s_screen, net_mode(), s_screen_off ? 2 : s_dim ? 1 : 0, app_brightness_pct());

    store_flush();
}
