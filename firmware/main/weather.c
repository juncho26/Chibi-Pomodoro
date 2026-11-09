// Today's rain forecast. Uses Open-Meteo (https://open-meteo.com): free, no account or key.
// The device joins home Wi-Fi quietly about once an hour, fetches 48 hours of hourly rain chance
// and amount, then turns Wi-Fi off again. Nothing is fetched without home Wi-Fi and a place.
#include "weather.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "store.h"
#include "net.h"
#include "rtc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "weather";

#define RAIN_PROB   10      // even a slight chance (10 %) in an hour counts as "going to rain"
#define RAIN_MM10   1       // or any measurable amount (0.1 mm) forecast in an hour

// Kept across deep sleep (not across power-off).
typedef struct {
    uint32_t magic;
    int32_t  day0;          // local day number (days since 1970) of hour 0
    uint8_t  prob[48];      // % chance per hour
    uint8_t  mm10[48];      // precipitation per hour, tenths of a mm (capped at 255)
    uint8_t  snow[48];      // 1 = falls as snow that hour
    int64_t  fetched;       // epoch seconds
} wx_cache_t;
static RTC_DATA_ATTR wx_cache_t s_c;
#define MAGIC 0x57583032

static volatile bool s_poke = true;
static char s_status[96] = "Not set up";

void weather_poke(void) { s_poke = true; }
void weather_status(char *buf, int len) { snprintf(buf, len, "%s", s_status); }

static int local_day(time_t t, int *hour)
{
    struct tm lt; localtime_r(&t, &lt);
    if (hour) *hour = lt.tm_hour;
    // days since 1970 of the local date
    int y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
    y -= m <= 2; int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool weather_rain_today(rain_info_t *o)
{
    memset(o, 0, sizeof *o);
    if (s_c.magic != MAGIC || !rtc_time_valid()) return false;
    int hour, day = local_day(time(NULL), &hour);
    int base = (day - s_c.day0) * 24;
    if (base < 0 || base + 23 >= 48) return false;           // forecast is from another day
    o->from_hour = -1;
    int mm_max = 0, snow_h = 0, wet_h = 0;
    for (int h = hour; h < 24; h++) {
        int i = base + h;
        bool wet = s_c.prob[i] >= RAIN_PROB || s_c.mm10[i] >= RAIN_MM10;
        if (s_c.prob[i] > o->max_prob) o->max_prob = s_c.prob[i];
        if (wet) {
            if (o->from_hour < 0) o->from_hour = h;
            if (s_c.mm10[i] > mm_max) mm_max = s_c.mm10[i];
            wet_h++; snow_h += s_c.snow[i];
        }
    }
    o->rain = o->from_hour >= 0;
    o->now = o->rain && o->from_hour == hour;
    if (o->rain) {
        // snow if it's snow now (or in the first wet hour), else if most wet hours are snow
        o->snow = s_c.snow[base + o->from_hour] || snow_h * 2 > wet_h;
        // a slight chance is a light drizzle; a high chance or a big amount pours
        int i = base + (o->now ? hour : o->from_hour);
        float p = (o->now ? s_c.prob[i] : o->max_prob) / 100.f, mm = (o->now ? s_c.mm10[i] : mm_max) / 10.f;
        o->intensity = fminf(1.f, .12f + p * .6f + mm * .12f);
        if (!o->now) o->intensity = fminf(o->intensity, .55f);
    }
    return true;
}


// ------------------------------------------------------------------ HTTP
static char *http_get(const char *url, int max)
{
    // buffer_size_tx/buffer_size: room for long response headers (some services send many)
    esp_http_client_config_t c = { .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 6144, .buffer_size_tx = 1024 };   // over 4 KB: allocated in PSRAM
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    char *buf = NULL;
    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "couldn't reach %.40s: %s (free RAM %u, largest %u)", url, esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    else {
        esp_http_client_fetch_headers(h);
        int code = esp_http_client_get_status_code(h);
        if (code != 200) ESP_LOGW(TAG, "%.40s answered %d", url, code);
        if (code == 200) {
            buf = malloc(max + 1);
            int got = 0, k;
            while (buf && got < max && (k = esp_http_client_read(h, buf + got, max - got)) > 0) got += k;
            if (buf) buf[got] = 0;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return buf;
}

static void url_encode(const char *s, char *out, int len)
{
    int o = 0;
    for (; *s && o < len - 4; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = c;
        else if (c == ' ') { out[o++] = '%'; out[o++] = '2'; out[o++] = '0'; }
        else o += snprintf(out + o, len - o, "%%%02X", c);
    }
    out[o] = 0;
}

// "35.69, 139.69" typed directly works too
static bool parse_coords(const char *s, float *lat, float *lon)
{
    char *e;
    float a = strtof(s, &e); if (e == s) return false;
    while (*e == ' ' || *e == ',') e++;
    char *e2; float b = strtof(e, &e2); if (e2 == e) return false;
    while (*e2 == ' ') e2++;
    if (*e2 || fabsf(a) > 90 || fabsf(b) > 180) return false;
    *lat = a; *lon = b; return true;
}

static bool geocode(wxcfg_t *w)
{
    float la, lo;
    if (parse_coords(w->city, &la, &lo)) { w->lat = la; w->lon = lo; snprintf(w->place, sizeof w->place, "%.2f, %.2f", la, lo); return true; }
    char q[200], url[300];
    url_encode(w->city, q, sizeof q);
    snprintf(url, sizeof url, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json", q);
    char *b = http_get(url, 4096);
    if (!b) return false;
    cJSON *j = cJSON_Parse(b); free(b);
    const cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "results"), 0);
    bool ok = false;
    if (r) {
        const cJSON *a = cJSON_GetObjectItem(r, "latitude"), *o = cJSON_GetObjectItem(r, "longitude");
        const cJSON *n = cJSON_GetObjectItem(r, "name"), *ad = cJSON_GetObjectItem(r, "admin1");
        if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) {
            w->lat = a->valuedouble; w->lon = o->valuedouble; ok = true;
            if (cJSON_IsString(ad) && cJSON_IsString(n) && strcmp(ad->valuestring, n->valuestring))
                snprintf(w->place, sizeof w->place, "%s, %s", n->valuestring, ad->valuestring);
            else snprintf(w->place, sizeof w->place, "%s", cJSON_IsString(n) ? n->valuestring : w->city);
        }
    } else snprintf(w->place, sizeof w->place, "?");          // searched, nothing found
    cJSON_Delete(j);
    return ok;
}

// Approximate place from the internet connection (no GPS on the board, and phones only share
// their location with https pages). City-level on home internet; rougher on a phone hotspot.
static bool ip_locate(wxcfg_t *w)
{
    char *b = http_get("https://ipinfo.io/json", 2048);
    bool ok = false;
    if (b) {
        cJSON *j = cJSON_Parse(b); free(b);
        const cJSON *loc = cJSON_GetObjectItem(j, "loc"), *ci = cJSON_GetObjectItem(j, "city"), *rg = cJSON_GetObjectItem(j, "region");
        float la, lo;
        if (cJSON_IsString(loc) && sscanf(loc->valuestring, "%f,%f", &la, &lo) == 2) {
            w->lat = la; w->lon = lo; ok = true;
            snprintf(w->place, sizeof w->place, "%s%s%s", cJSON_IsString(ci) ? ci->valuestring : "", cJSON_IsString(ci) && cJSON_IsString(rg) ? ", " : "", cJSON_IsString(rg) ? rg->valuestring : "");
        }
        cJSON_Delete(j);
    }
    if (ok) { ESP_LOGI(TAG, "location from ipinfo.io: %s", w->place); return true; }
    // second service: ip-api.com (plain http, so it works even when a secure connection can't be made)
    b = http_get("http://ip-api.com/json/?fields=status,lat,lon,city,regionName", 1024);
    if (b) {
        cJSON *j = cJSON_Parse(b); free(b);
        const cJSON *la = cJSON_GetObjectItem(j, "lat"), *lo = cJSON_GetObjectItem(j, "lon"), *ci = cJSON_GetObjectItem(j, "city"), *rg = cJSON_GetObjectItem(j, "regionName");
        if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            w->lat = la->valuedouble; w->lon = lo->valuedouble; ok = true;
            snprintf(w->place, sizeof w->place, "%s%s%s", cJSON_IsString(ci) ? ci->valuestring : "", cJSON_IsString(ci) && cJSON_IsString(rg) ? ", " : "", cJSON_IsString(rg) ? rg->valuestring : "");
        }
        cJSON_Delete(j);
    }
    if (ok) { ESP_LOGI(TAG, "location from ip-api.com: %s", w->place); return true; }
    b = http_get("https://ipapi.co/json/", 4096);          // third service
    if (!b) return false;
    cJSON *j = cJSON_Parse(b); free(b);
    const cJSON *la = cJSON_GetObjectItem(j, "latitude"), *lo = cJSON_GetObjectItem(j, "longitude"), *ci = cJSON_GetObjectItem(j, "city"), *rg = cJSON_GetObjectItem(j, "region");
    if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
        w->lat = la->valuedouble; w->lon = lo->valuedouble; ok = true;
        snprintf(w->place, sizeof w->place, "%s%s%s", cJSON_IsString(ci) ? ci->valuestring : "", cJSON_IsString(ci) && cJSON_IsString(rg) ? ", " : "", cJSON_IsString(rg) ? rg->valuestring : "");
    }
    cJSON_Delete(j);
    return ok;
}

static bool fetch_forecast(float lat, float lon)
{
    char url[256];
    snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&hourly=precipitation_probability,precipitation,snowfall,weather_code,temperature_2m&forecast_days=2&timezone=auto&timeformat=unixtime", lat, lon);
    char *b = http_get(url, 12000);
    if (!b) return false;
    cJSON *j = cJSON_Parse(b); free(b);
    cJSON *hr = cJSON_GetObjectItem(j, "hourly");
    cJSON *tm = cJSON_GetObjectItem(hr, "time"), *pp = cJSON_GetObjectItem(hr, "precipitation_probability"), *pr = cJSON_GetObjectItem(hr, "precipitation");
    cJSON *sf = cJSON_GetObjectItem(hr, "snowfall"), *wc = cJSON_GetObjectItem(hr, "weather_code"), *tp = cJSON_GetObjectItem(hr, "temperature_2m");
    bool ok = cJSON_IsArray(tm) && cJSON_GetArraySize(tm) >= 24;
    if (ok) {
        wx_cache_t c = { .magic = MAGIC };
        // the first entry is local midnight at the place; use our own clock's day numbering for it
        c.day0 = local_day((time_t)cJSON_GetArrayItem(tm, 0)->valuedouble + 3600, NULL);
        int n = cJSON_GetArraySize(tm); if (n > 48) n = 48;
        for (int i = 0; i < n; i++) {
            const cJSON *p = cJSON_GetArrayItem(pp, i), *m = cJSON_GetArrayItem(pr, i);
            c.prob[i] = cJSON_IsNumber(p) ? (uint8_t)fminf(100, fmaxf(0, p->valuedouble)) : 0;
            c.mm10[i] = cJSON_IsNumber(m) ? (uint8_t)fminf(255, fmaxf(0, m->valuedouble * 10 + .5f)) : 0;
            // snow: snow weather codes (71-77, 85, 86), forecast snowfall, or a chance at freezing temperatures
            const cJSON *s = cJSON_GetArrayItem(sf, i), *w = cJSON_GetArrayItem(wc, i), *te = cJSON_GetArrayItem(tp, i);
            int code = cJSON_IsNumber(w) ? w->valueint : 0;
            bool sn = (code >= 71 && code <= 77) || code == 85 || code == 86 || (cJSON_IsNumber(s) && s->valuedouble > 0);
            if (!sn && cJSON_IsNumber(te) && te->valuedouble <= 1.0 && (code < 51 || code > 67)) sn = true;
            c.snow[i] = sn;
        }
        c.fetched = time(NULL);
        s_c = c;
    }
    cJSON_Delete(j);
    return ok;
}

static void task(void *arg)
{
    int64_t next_try = 0;
    vTaskDelay(pdMS_TO_TICKS(8000));        // let the first screen settle
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        wxcfg_t w; int nwifi;
        store_lock(); w = *store_wx(); nwifi = store_wifis()->n; store_unlock();
        bool configured = w.on && nwifi;
        if (!w.on) snprintf(s_status, sizeof s_status, "Not set up");
        else if (!nwifi) snprintf(s_status, sizeof s_status, "Needs a Wi-Fi network");
        bool stale = s_c.magic != MAGIC || !rtc_time_valid() || time(NULL) - s_c.fetched > 3600 || time(NULL) < s_c.fetched;
        if (s_poke) { s_poke = false; next_try = 0; if (!w.located) stale = true; }
        // once an hour (or right after a settings change)
        if (configured && (stale || !w.located) && now >= next_try) {
            if (!net_sta_acquire(25000)) {
                if (net_mode() != NET_AP) snprintf(s_status, sizeof s_status, "No saved Wi-Fi in range");
                next_try = now + 10 * 60000;
            } else {
                // auto: locate again on every check, so it follows you between home and a phone hotspot
                bool ok = w.located && !w.autoloc;
                if (!ok) {
                    wxcfg_t g = w;
                    ok = w.autoloc ? ip_locate(&g) : geocode(&g);
                    store_lock();
                    wxcfg_t *sw = store_wx();
                    if (sw->on == w.on && sw->autoloc == w.autoloc && !strcmp(sw->city, w.city)) {   // unchanged meanwhile?
                        snprintf(sw->place, sizeof sw->place, "%s", g.place);
                        if (ok) { sw->lat = g.lat; sw->lon = g.lon; sw->located = 1; }
                        else if (!w.autoloc) sw->located = 0;
                        store_mark_wx();
                    }
                    store_unlock();
                    if (ok) w = g;
                    else if (w.autoloc && w.located) ok = true;        // use where we were last time
                    if (!ok) snprintf(s_status, sizeof s_status, w.autoloc ? "Couldn't estimate the location" :
                                      strcmp(g.place, "?") ? "Couldn't look up the place" : "Place not found. Try another name");
                }
                if (ok) {
                    if (fetch_forecast(w.lat, w.lon)) {
                        time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
                        snprintf(s_status, sizeof s_status, "Updated %02d:%02d", lt.tm_hour, lt.tm_min);
                        rain_info_t ri; bool r = weather_rain_today(&ri) && ri.rain;
                        ESP_LOGI(TAG, "forecast updated for %s: %s", w.place,
                                 !r ? "no rain today" : ri.now ? (ri.snow ? "snowing now" : "raining now") : (ri.snow ? "snow later today" : "rain later today"));
                        next_try = now + 60 * 60000;
                    } else { snprintf(s_status, sizeof s_status, "Couldn't get the forecast"); next_try = now + 10 * 60000; }
                } else next_try = now + 10 * 60000;
                net_sta_release();
            }
        }
        { static char said[sizeof s_status]; if (strcmp(said, s_status)) { strcpy(said, s_status); ESP_LOGI(TAG, "weather: %s", s_status); } }
        for (int i = 0; i < 30 && !s_poke; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Stack in PSRAM (it never writes to flash): keeps 8 KB of internal RAM free for Wi-Fi.
void weather_start(void) { xTaskCreateWithCaps(task, "weather", 8192, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
