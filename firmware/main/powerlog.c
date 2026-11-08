// See powerlog.h. Everything here is a handful of additions per frame; nothing touches flash.
//
// Plugging into a computer pauses the count and prints a battery report to the serial log (once
// right away, then every 30 s), so it can be read in a serial console. A short plug-in only pauses
// it; staying plugged in for 10 minutes or more counts as charging, and the next unplug starts a
// new count.
#include "powerlog.h"
#include "power.h"
#include "app.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"

static const char *TAG = "battery";

#define PL_MAGIC 0x50574C32
#define NSCR 9
#define CHARGE_MS (10 * 60000)          // plugged in this long: treat it as a charge
static const char *SCR_NAME[NSCR] = { "home", "study", "done", "break", "tasks", "clear", "carry", "settings", "direction" };

typedef struct {
    uint32_t magic;
    uint64_t start_us;              // RTC clock when counting started (it keeps running in deep sleep)
    uint64_t usb_since_us;          // plugged into a computer since (0 = on battery)
    uint64_t usb_total_ms;          // short plug-ins, left out of the time on battery
    uint32_t awake_ms;
    uint32_t scr_ms[NSCR];
    uint32_t light_ms[3];           // lit, dimmed, off
    uint32_t wifi_ms, ap_ms;        // Wi-Fi on at all · of which the Chibi-XXXX hotspot
    uint64_t bright_ms;             // brightness % × time, while lit (for the average)
    int32_t  start_mv, last_mv;     // battery at the start · last reading on battery (charging raises it)
    uint32_t sleeps;
} pl_t;
RTC_NOINIT_ATTR static pl_t P;

static void pl_reset(int mv)
{
    memset(&P, 0, sizeof P);
    P.magic = PL_MAGIC;
    P.start_us = esp_rtc_get_time_us();
    P.start_mv = P.last_mv = mv;
}

static int pct(uint32_t part, uint32_t whole) { return whole ? (int)((part * 100ULL + whole / 2) / whole) : 0; }

typedef struct { float hours, asleep_h, projected_h; int now_mv, start_pct, now_pct; } summary_t;

static summary_t summarize(void)
{
    summary_t s = { 0 };
    uint64_t end_us = P.usb_since_us ? P.usb_since_us : esp_rtc_get_time_us();
    uint64_t elapsed_ms = (end_us - P.start_us) / 1000;
    elapsed_ms = elapsed_ms > P.usb_total_ms ? elapsed_ms - P.usb_total_ms : 0;
    if (elapsed_ms < P.awake_ms) elapsed_ms = P.awake_ms;
    s.hours = elapsed_ms / 3.6e6f;
    s.asleep_h = (elapsed_ms - P.awake_ms) / 3.6e6f;
    s.now_mv = P.usb_since_us ? P.last_mv : power_battery_mv();
    s.start_pct = P.start_mv > 0 ? power_pct_from_mv(P.start_mv) : -1;
    s.now_pct = s.now_mv > 0 ? power_pct_from_mv(s.now_mv) : -1;
    int drop = s.start_pct - s.now_pct;
    // the % comes from the voltage, so it is rough; only project once a fair amount has been used
    s.projected_h = (s.start_pct >= 0 && s.now_pct >= 0 && drop >= 8 && s.hours >= 1) ? s.hours * 100.f / drop : 0;
    return s;
}

static void report(void)
{
    summary_t s = summarize();
    uint32_t a = P.awake_ms, lit = P.light_ms[0];
    char scr[160]; int n = 0; scr[0] = 0;
    for (int i = 0; i < NSCR && n < (int)sizeof scr - 24; i++) {
        int p = pct(P.scr_ms[i], a);
        if (p) n += snprintf(scr + n, sizeof scr - n, " %s %d%%", SCR_NAME[i], p);
    }
    ESP_LOGI(TAG, "======== battery report (counting paused while plugged in) ========");
    ESP_LOGI(TAG, "time on battery : %.1f h (of which %.1f h in deep sleep)", s.hours, s.asleep_h);
    ESP_LOGI(TAG, "Wi-Fi on        : %d%% of the time (hotspot %d%%)", pct(P.wifi_ms, a), pct(P.ap_ms, a));
    ESP_LOGI(TAG, "screen          : bright %d%% (at %d%% brightness), dimmed %d%%, off %d%%",
             pct(lit, a), lit ? (int)(P.bright_ms / lit) : 0, pct(P.light_ms[1], a), pct(P.light_ms[2], a));
    ESP_LOGI(TAG, "screens         :%s", n ? scr : " -");
    ESP_LOGI(TAG, "battery         : %d mV (%d%%) -> %d mV (%d%%)", (int)P.start_mv, s.start_pct, s.now_mv, s.now_pct);
    if (s.projected_h > 0) ESP_LOGI(TAG, "full charge     : about %.0f h at this rate", s.projected_h);
    else ESP_LOGI(TAG, "full charge     : not enough used yet to estimate (needs about 8%% and 1 h)");
    ESP_LOGI(TAG, "====================================================================");
}

void powerlog_tick(int dt_ms, int screen, int net_mode, int light, int bright_pct)
{
    int mv = power_battery_mv();
    uint64_t now_us = esp_rtc_get_time_us();
    if (P.magic != PL_MAGIC) pl_reset(mv);

    if (power_on_usb()) {                       // plugged into a computer: pause and report
        static uint64_t next_report;
        if (!P.usb_since_us) { P.usb_since_us = now_us; next_report = 0; }
        if (now_us >= next_report) { next_report = now_us + 30000000ULL; report(); }
        return;
    }
    if (P.usb_since_us) {                       // just unplugged
        uint64_t plugged_ms = (now_us - P.usb_since_us) / 1000;
        if (plugged_ms >= CHARGE_MS) { ESP_LOGI(TAG, "charged: starting a new count"); pl_reset(mv); return; }
        P.usb_total_ms += plugged_ms;
        P.usb_since_us = 0;
    }
    if (P.start_mv <= 0 && mv > 0) P.start_mv = mv;                       // first battery reading arrived
    else if (mv > 0 && mv > P.start_mv + 150) { pl_reset(mv); return; }   // charged with a plain USB charger
    if (mv > 0) P.last_mv = mv;
    if (dt_ms <= 0) return;
    P.awake_ms += dt_ms;
    if (screen >= 0 && screen < NSCR) P.scr_ms[screen] += dt_ms;
    if (light >= 0 && light < 3) P.light_ms[light] += dt_ms;
    if (light == 0) P.bright_ms += (uint64_t)bright_pct * dt_ms;
    if (net_mode != NET_OFF) P.wifi_ms += dt_ms;
    if (net_mode == NET_AP) P.ap_ms += dt_ms;
}

void powerlog_note_sleep(void) { if (P.magic == PL_MAGIC) P.sleeps++; }

int powerlog_json(char *buf, int len)
{
    if (P.magic != PL_MAGIC) return snprintf(buf, len, "{\"counting\":false}");
    summary_t s = summarize();
    uint32_t a = P.awake_ms, lit = P.light_ms[0];
    int n = snprintf(buf, len,
        "{\"sinceUnpluggedHours\":%.2f,\"asleepHours\":%.2f,\"deepSleeps\":%lu,"
        "\"wifiOnPct\":%d,\"hotspotPct\":%d,\"screenLitPct\":%d,\"avgBrightnessPct\":%d,\"screenDimPct\":%d,\"screenOffPct\":%d,"
        "\"startMv\":%d,\"nowMv\":%d,\"startPct\":%d,\"nowPct\":%d,\"projectedFullHours\":%.0f,\"screens\":{",
        s.hours, s.asleep_h, (unsigned long)P.sleeps,
        pct(P.wifi_ms, a), pct(P.ap_ms, a), pct(lit, a), lit ? (int)(P.bright_ms / lit) : 0, pct(P.light_ms[1], a), pct(P.light_ms[2], a),
        (int)P.start_mv, s.now_mv, s.start_pct, s.now_pct, s.projected_h);
    for (int i = 0, first = 1; i < NSCR && n < len - 24; i++) {
        int p = pct(P.scr_ms[i], a);
        if (!p) continue;
        n += snprintf(buf + n, len - n, "%s\"%s\":%d", first ? "" : ",", SCR_NAME[i], p);
        first = 0;
    }
    if (n < len - 3) n += snprintf(buf + n, len - n, "}}");
    return n;
}
