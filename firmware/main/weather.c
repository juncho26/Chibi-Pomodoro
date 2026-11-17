// Today's rain forecast. Uses Open-Meteo (https://open-meteo.com): free, no account or key.
// The device joins home Wi-Fi quietly about once an hour, fetches 48 hours of hourly rain chance
// and amount, then turns Wi-Fi off again. Nothing is fetched without home Wi-Fi and a place.
#include "weather.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
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
// When Open-Meteo last failed to answer (epoch s, 0 = it answered). Some home networks can't reach
// it at all; then the backups are asked first for 3 hours instead of waiting ~45 s for it every hour.
static RTC_DATA_ATTR int64_t s_om_failed;
#define OM_RETRY_S (3 * 3600)
static char s_status[96] = "Not set up";

void weather_poke(void) { s_poke = true; snprintf(s_status, sizeof s_status, "Checking\u2026"); }
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
static char *http_get_once(const char *url, int max, esp_err_t *out_err)
{
    // buffer_size_tx/buffer_size: room for long response headers (some services send many)
    esp_http_client_config_t c = { .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 6144, .buffer_size_tx = 1024,
                                   .addr_type = HTTP_ADDR_TYPE_INET,     // IPv4 only: the board has no IPv6 address
                                   .user_agent = "Chibi/1.0 (+https://github.com/juncho26/Chibi-Pomodoro)" };   // over 4 KB: allocated in PSRAM
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    char *buf = NULL;
    esp_err_t err = esp_http_client_open(h, 0);
    *out_err = err;
    if (err != ESP_OK) {
        char host[64] = ""; const char *hs = strstr(url, "://"); hs = hs ? hs + 3 : url;
        int hl = strcspn(hs, "/:?"); if (hl > 63) hl = 63; memcpy(host, hs, hl); host[hl] = 0;
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
        int64_t t0 = esp_timer_get_time();
        int gr = getaddrinfo(host, NULL, &hints, &ai);
        char ip[16] = "?";
        if (gr == 0 && ai) inet_ntoa_r(((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
        ESP_LOGW(TAG, "%s is at %s (looked up in %d ms%s)", host, ip, (int)((esp_timer_get_time() - t0) / 1000), gr ? ", lookup failed" : "");
        if (ai) freeaddrinfo(ai);
    }
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

// A connection that doesn't open is tried once more. For Open-Meteo, if the secure connection
// still won't open, the same request goes over plain http (Open-Meteo answers both; only the place's
// coordinates are sent, no personal data).
static char *http_get(const char *url, int max)
{
    esp_err_t err = ESP_OK;
    char *b = http_get_once(url, max, &err);
    if (!b && err != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(1000)); b = http_get_once(url, max, &err); }
    if (!b && err != ESP_OK && !strncmp(url, "https://", 8) && strstr(url, "open-meteo.com/")) {
        size_t n = strlen(url) + 1;
        char *plain = malloc(n);
        if (plain) {
            snprintf(plain, n, "http://%s", url + 8);
            ESP_LOGW(TAG, "secure connection didn't open: trying plain http");
            b = http_get_once(plain, max, &err);
            free(plain);
        }
    }
    return b;
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
// Backup forecast: the Norwegian Meteorological Institute (api.met.no, free, worldwide, no account;
// it asks every app to name itself, which the User-Agent above does). Used when Open-Meteo can't be
// reached. It gives rain/snow amounts and a weather symbol per hour but no rain chance, so the
// chance is estimated from those.
static int64_t utc_epoch(const char *s)            // "2026-11-18T03:00:00Z"
{
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
    y -= mo <= 2; int era = y / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}
static bool fetch_forecast_metno(float lat, float lon)
{
    char url[160];
    snprintf(url, sizeof url, "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.3f&lon=%.3f", lat, lon);
    char *b = http_get(url, 200000);                 // ~50-100 KB; read into PSRAM
    if (!b) { ESP_LOGW(TAG, "backup forecast: no answer"); return false; }
    cJSON *j = cJSON_Parse(b);
    if (!j) ESP_LOGW(TAG, "backup forecast: couldn't read the answer (%d bytes)", (int)strlen(b));
    free(b);
    cJSON *ts = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "properties"), "timeseries");
    bool ok = cJSON_IsArray(ts) && cJSON_GetArraySize(ts) >= 12;
    if (ok) {
        time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
        int64_t midnight = (int64_t)now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);   // local midnight today
        wx_cache_t c = { .magic = MAGIC };
        c.day0 = local_day(now, NULL);
        int filled = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, ts) {
            const cJSON *tm = cJSON_GetObjectItem(e, "time");
            if (!cJSON_IsString(tm)) continue;
            int64_t t = utc_epoch(tm->valuestring);
            if (t < 0) continue;
            int i = (int)((t - midnight) / 3600);
            if (i < 0) continue;
            if (i >= 48) break;
            const cJSON *dt = cJSON_GetObjectItem(e, "data");
            const cJSON *n1 = cJSON_GetObjectItem(dt, "next_1_hours");
            if (!n1) continue;                                   // past the hourly part
            const cJSON *sym = cJSON_GetObjectItem(cJSON_GetObjectItem(n1, "summary"), "symbol_code");
            const cJSON *pa = cJSON_GetObjectItem(cJSON_GetObjectItem(n1, "details"), "precipitation_amount");
            const cJSON *te = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(dt, "instant"), "details"), "air_temperature");
            float mm = cJSON_IsNumber(pa) ? (float)pa->valuedouble : 0;
            const char *sc = cJSON_IsString(sym) ? sym->valuestring : "";
            bool wet_sym = strstr(sc, "rain") || strstr(sc, "sleet") || strstr(sc, "snow") || strstr(sc, "showers");
            c.mm10[i] = (uint8_t)fminf(255, fmaxf(0, mm * 10 + .5f));
            c.prob[i] = mm >= .1f ? (uint8_t)fminf(100, 50 + mm * 50) : wet_sym ? 30 : 0;   // estimated chance
            c.snow[i] = strstr(sc, "snow") || strstr(sc, "sleet") || (cJSON_IsNumber(te) && te->valuedouble <= 1.0 && (mm > 0 || wet_sym));
            filled++;
        }
        ok = filled >= 6;
        if (ok) { c.fetched = now; s_c = c; }
        else ESP_LOGW(TAG, "backup forecast: answer has too few hours (%d)", filled);
    } else if (j) ESP_LOGW(TAG, "backup forecast: answer has no forecast data");
    cJSON_Delete(j);
    return ok;
}

// Backup forecast in Japan: the Japan Meteorological Agency (気象庁), the same data its own forecast
// page uses (www.jma.go.jp/bosai, no account; "出典：気象庁ホームページ"). It gives the real rain chance
// (降水確率) in 6-hour blocks for today and tomorrow, and each day's weather in words, which tells
// rain from snow. The forecast is per prefecture area: the office nearest to the place is used.
typedef struct { float lat, lon; const char *office, *area; } jma_office_t;   // area NULL: the office's first area
static const jma_office_t JMA_OFFICES[] = {
    { 45.41f, 141.67f, "011000", NULL },     { 43.77f, 142.36f, "012000", NULL },     { 44.02f, 144.27f, "013000", NULL },
    { 42.98f, 144.38f, "014100", "014100" }, { 42.92f, 143.20f, "014100", "014030" }, { 42.32f, 140.97f, "015000", NULL },
    { 43.06f, 141.35f, "016000", NULL },     { 41.77f, 140.73f, "017000", NULL },
    { 40.82f, 140.74f, "020000", NULL }, { 39.70f, 141.15f, "030000", NULL }, { 38.27f, 140.87f, "040000", NULL },
    { 39.72f, 140.10f, "050000", NULL }, { 38.24f, 140.36f, "060000", NULL }, { 37.75f, 140.47f, "070000", NULL },
    { 36.34f, 140.45f, "080000", NULL }, { 36.57f, 139.88f, "090000", NULL }, { 36.39f, 139.06f, "100000", NULL },
    { 35.86f, 139.65f, "110000", NULL }, { 35.61f, 140.12f, "120000", NULL }, { 35.69f, 139.69f, "130000", NULL },
    { 35.45f, 139.64f, "140000", NULL }, { 37.90f, 139.02f, "150000", NULL }, { 36.70f, 137.21f, "160000", NULL },
    { 36.59f, 136.63f, "170000", NULL }, { 36.07f, 136.22f, "180000", NULL }, { 35.66f, 138.57f, "190000", NULL },
    { 36.65f, 138.18f, "200000", NULL }, { 35.39f, 136.72f, "210000", NULL }, { 34.98f, 138.38f, "220000", NULL },
    { 35.18f, 136.91f, "230000", NULL }, { 34.73f, 136.51f, "240000", NULL }, { 35.00f, 135.87f, "250000", NULL },
    { 35.02f, 135.76f, "260000", NULL }, { 34.69f, 135.52f, "270000", NULL }, { 34.69f, 135.18f, "280000", NULL },
    { 34.69f, 135.83f, "290000", NULL }, { 34.23f, 135.17f, "300000", NULL }, { 35.50f, 134.24f, "310000", NULL },
    { 35.47f, 133.05f, "320000", NULL }, { 34.66f, 133.93f, "330000", NULL }, { 34.40f, 132.46f, "340000", NULL },
    { 34.19f, 131.47f, "350000", NULL }, { 34.07f, 134.56f, "360000", NULL }, { 34.34f, 134.04f, "370000", NULL },
    { 33.84f, 132.77f, "380000", NULL }, { 33.56f, 133.53f, "390000", NULL }, { 33.61f, 130.42f, "400000", NULL },
    { 33.25f, 130.30f, "410000", NULL }, { 32.74f, 129.87f, "420000", NULL }, { 32.79f, 130.74f, "430000", NULL },
    { 33.24f, 131.61f, "440000", NULL }, { 31.91f, 131.42f, "450000", NULL },
    { 31.56f, 130.56f, "460100", "460100" }, { 28.38f, 129.49f, "460100", "460040" },
    { 26.21f, 127.68f, "471000", NULL }, { 25.83f, 131.23f, "472000", NULL }, { 24.80f, 125.28f, "473000", NULL },
    { 24.34f, 124.16f, "474000", NULL },
};

// The nearest forecast office, or NULL when the place isn't in Japan (more than ~170 km from every
// office, e.g. Korea, or outside the box around Japan).
static const jma_office_t *jma_office(float lat, float lon)
{
    if (lat < 24 || lat > 45.6f || lon < 122.9f || lon > 146) return NULL;
    const jma_office_t *best = NULL; float bd = 1e9f, k = cosf(lat * (float)M_PI / 180);
    for (size_t i = 0; i < sizeof JMA_OFFICES / sizeof *JMA_OFFICES; i++) {
        float dy = lat - JMA_OFFICES[i].lat, dx = (lon - JMA_OFFICES[i].lon) * k, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = &JMA_OFFICES[i]; }
    }
    return bd <= 1.7f * 1.7f ? best : NULL;
}

static int64_t jma_epoch(const char *s)            // "2026-11-18T05:00:00+09:00"
{
    int64_t t = utc_epoch(s);
    const char *z = s ? strpbrk(s + 19, "+-Z") : NULL;
    int oh = 0, om = 0;
    if (t >= 0 && z && *z != 'Z' && sscanf(z + 1, "%d:%d", &oh, &om) >= 1) t -= (*z == '-' ? -1 : 1) * (oh * 3600 + om * 60);
    return t;
}

// The area entry of one time series: the wanted area code, else the first one.
static const cJSON *jma_area(const cJSON *series, const char *code)
{
    const cJSON *areas = cJSON_GetObjectItem(series, "areas"), *a;
    if (code) cJSON_ArrayForEach(a, areas) {
        const cJSON *c = cJSON_GetObjectItem(cJSON_GetObjectItem(a, "area"), "code");
        if (cJSON_IsString(c) && !strcmp(c->valuestring, code)) return a;
    }
    return cJSON_GetArrayItem(areas, 0);
}

static bool fetch_forecast_jma(const jma_office_t *of)
{
    char url[96];
    snprintf(url, sizeof url, "https://www.jma.go.jp/bosai/forecast/data/forecast/%s.json", of->office);
    char *b = http_get(url, 65536);                  // ~10-30 KB
    if (!b) { ESP_LOGW(TAG, "JMA forecast: no answer"); return false; }
    cJSON *j = cJSON_Parse(b);
    if (!j) ESP_LOGW(TAG, "JMA forecast: couldn't read the answer (%d bytes)", (int)strlen(b));
    free(b);
    // [0] = short-range forecast; its time series: [0] weather in words per day, [1] rain chance per 6 h
    const cJSON *ts = cJSON_GetObjectItem(cJSON_GetArrayItem(j, 0), "timeSeries");
    const cJSON *wx = NULL, *pop = NULL, *s;
    cJSON_ArrayForEach(s, ts) {                       // found by their contents, not their position
        const cJSON *a = jma_area(s, of->area);
        if (!wx && cJSON_GetObjectItem(a, "weathers")) wx = s;
        if (!pop && cJSON_GetObjectItem(a, "pops")) pop = s;
    }
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    int64_t midnight = (int64_t)now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    wx_cache_t c = { .magic = MAGIC };
    c.day0 = local_day(now, NULL);
    int blocks = 0;
    if (pop) {
        const cJSON *td = cJSON_GetObjectItem(pop, "timeDefines"), *pp = cJSON_GetObjectItem(jma_area(pop, of->area), "pops");
        int n = cJSON_GetArraySize(td);
        for (int k = 0; k < n; k++) {
            const cJSON *t = cJSON_GetArrayItem(td, k), *p = cJSON_GetArrayItem(pp, k);
            if (!cJSON_IsString(t) || !cJSON_IsString(p) || !p->valuestring[0]) continue;     // "" = no value
            int64_t e = jma_epoch(t->valuestring);
            if (e < 0) continue;
            int64_t next = k + 1 < n && cJSON_IsString(cJSON_GetArrayItem(td, k + 1)) ? jma_epoch(cJSON_GetArrayItem(td, k + 1)->valuestring) : e + 6 * 3600;
            if (next <= e || next - e > 12 * 3600) next = e + 6 * 3600;
            int pct = atoi(p->valuestring); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            for (int64_t h = e; h < next; h += 3600) {
                int i = (int)((h - midnight) / 3600);
                if (h >= midnight && i < 48) c.prob[i] = (uint8_t)pct;
            }
            blocks++;
        }
    }
    if (wx) {                                         // snow: a day whose weather is mostly snow
        const cJSON *td = cJSON_GetObjectItem(wx, "timeDefines"), *ws = cJSON_GetObjectItem(jma_area(wx, of->area), "weathers");
        const cJSON *wc = cJSON_GetObjectItem(jma_area(wx, of->area), "weatherCodes");
        for (int k = 0; k < cJSON_GetArraySize(td); k++) {
            const cJSON *t = cJSON_GetArrayItem(td, k), *w = cJSON_GetArrayItem(ws, k), *code = cJSON_GetArrayItem(wc, k);
            if (!cJSON_IsString(t)) continue;
            int64_t e = jma_epoch(t->valuestring);
            if (e < 0) continue;
            int day = e < midnight ? 0 : (int)((e - midnight) / 86400);
            if (day > 1) continue;
            const char *txt = cJSON_IsString(w) ? w->valuestring : "";
            const char *yuki = strstr(txt, "雪"), *ame = strstr(txt, "雨");
            // weather codes 400-499 are snow; in words, snow mentioned before rain ("雪か雨")
            bool sn = (cJSON_IsString(code) && code->valuestring[0] == '4') || (yuki && (!ame || yuki < ame));
            if (sn) memset(c.snow + day * 24, 1, 24);
        }
    }
    bool ok = blocks >= 2;
    if (ok) { c.fetched = now; s_c = c; }
    else if (j) ESP_LOGW(TAG, "JMA forecast: answer has no rain chances (%d)", blocks);
    cJSON_Delete(j);
    return ok;
}

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
    if (!b) { ESP_LOGW(TAG, "forecast: no answer"); return false; }
    int blen = strlen(b);
    cJSON *j = cJSON_Parse(b);
    if (!j) ESP_LOGW(TAG, "forecast: couldn't read the answer (%d bytes, starts \"%.60s\")", blen, b);
    free(b);
    cJSON *hr = cJSON_GetObjectItem(j, "hourly");
    cJSON *tm = cJSON_GetObjectItem(hr, "time"), *pp = cJSON_GetObjectItem(hr, "precipitation_probability"), *pr = cJSON_GetObjectItem(hr, "precipitation");
    cJSON *sf = cJSON_GetObjectItem(hr, "snowfall"), *wc = cJSON_GetObjectItem(hr, "weather_code"), *tp = cJSON_GetObjectItem(hr, "temperature_2m");
    bool ok = cJSON_IsArray(tm) && cJSON_GetArraySize(tm) >= 24;
    if (j && !ok) ESP_LOGW(TAG, "forecast: answer has no hourly data");
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
    int quick = 0;                           // failed checks in a row: the first few are retried after a minute
    vTaskDelay(pdMS_TO_TICKS(8000));        // let the first screen settle
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        wxcfg_t w; int nwifi;
        store_lock(); w = *store_wx(); nwifi = store_wifis()->n; store_unlock();
        bool configured = w.on && nwifi;
        if (!w.on) snprintf(s_status, sizeof s_status, "Not set up");
        else if (!nwifi) snprintf(s_status, sizeof s_status, "Needs a Wi-Fi network");
        bool stale = s_c.magic != MAGIC || !rtc_time_valid() || time(NULL) - s_c.fetched > 3600 || time(NULL) < s_c.fetched;
        // Save on the phone page: always check now, even if the forecast is recent (it used to skip
        // the check then, which left "Checking…" showing)
        if (s_poke) { s_poke = false; next_try = 0; quick = 0; stale = true; }
        // after a failure: try again in a minute (three times), then every 10 minutes
        const int64_t retry = quick < 3 ? 60000 : 10 * 60000;
        // once an hour (or right after a settings change)
        if (configured && (stale || !w.located) && now >= next_try) {
            bool was_on = net_mode() == NET_STA;
            if (!net_sta_acquire(25000)) {
                if (net_mode() != NET_AP) { snprintf(s_status, sizeof s_status, "No saved Wi-Fi in range"); quick++; }
                next_try = now + (net_mode() == NET_AP ? 10 * 60000 : retry);
            } else {
                // just joined: give the router a moment (address lookups right after joining were
                // sometimes lost, which made the first check after setup fail)
                if (!was_on) vTaskDelay(pdMS_TO_TICKS(1500));
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
                    // Open-Meteo first; then, in Japan, the Japan Meteorological Agency; then MET Norway.
                    // If Open-Meteo failed in the last 3 hours, it is asked last instead of first.
                    time_t tn = time(NULL);
                    bool om_first = !s_om_failed || tn < s_om_failed || tn - s_om_failed >= OM_RETRY_S;
                    bool got = false;
                    if (om_first) { got = fetch_forecast(w.lat, w.lon); s_om_failed = got ? 0 : tn; }
                    else ESP_LOGI(TAG, "Open-Meteo didn't answer recently: asking the backup first");
                    const jma_office_t *of = got ? NULL : jma_office(w.lat, w.lon);
                    if (of) {
                        if (om_first) ESP_LOGW(TAG, "Open-Meteo didn't answer: trying the backup forecast (JMA, office %s)", of->office);
                        got = fetch_forecast_jma(of);
                        if (got) ESP_LOGI(TAG, "forecast from JMA");
                    }
                    if (!got) {
                        if (of || om_first) ESP_LOGW(TAG, "%s didn't answer: trying the backup forecast (api.met.no)", of ? "JMA" : "Open-Meteo");
                        got = fetch_forecast_metno(w.lat, w.lon);
                        if (got) ESP_LOGI(TAG, "forecast from api.met.no");
                    }
                    if (!got && !om_first) {
                        ESP_LOGW(TAG, "backups didn't answer: trying Open-Meteo");
                        got = fetch_forecast(w.lat, w.lon);
                        if (got) s_om_failed = 0;
                    }
                    if (got) {
                        time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
                        snprintf(s_status, sizeof s_status, "Updated %02d:%02d", lt.tm_hour, lt.tm_min);
                        rain_info_t ri; bool r = weather_rain_today(&ri) && ri.rain;
                        ESP_LOGI(TAG, "forecast updated for %s: %s (highest chance for the rest of today %d%%)", w.place,
                                 !r ? "no rain today" : ri.now ? (ri.snow ? "snowing now" : "raining now") : (ri.snow ? "snow later today" : "rain later today"), ri.max_prob);
                        next_try = now + 60 * 60000; quick = 0;
                    } else { snprintf(s_status, sizeof s_status, "Couldn't get the forecast"); next_try = now + retry; quick++; }
                } else { next_try = now + retry; quick++; }
                net_sta_release();
            }
        }
        { static char said[sizeof s_status]; if (strcmp(said, s_status)) { strcpy(said, s_status); ESP_LOGI(TAG, "weather: %s", s_status); } }
        for (int i = 0; i < 30 && !s_poke; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void *json_alloc(size_t n) { void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return p ? p : malloc(n); }
static void json_free(void *p) { heap_caps_free(p); }

// Stack in PSRAM (it never writes to flash): keeps 8 KB of internal RAM free for Wi-Fi.
void weather_start(void) {
    // JSON (the forecast, the phone page's requests) is parsed into hundreds of small pieces; keep
    // them in PSRAM too. In the small internal RAM, the forecast sometimes couldn't be read at all.
    cJSON_Hooks hk = { .malloc_fn = json_alloc, .free_fn = json_free };
    cJSON_InitHooks(&hk); xTaskCreateWithCaps(task, "weather", 8192, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
