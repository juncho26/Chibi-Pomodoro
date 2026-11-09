// Local-only networking. Home Wi-Fi if saved (page at http://<ip>/?t=TOKEN and chibi.local),
// otherwise the device's own hotspot with a captive portal so the page opens by itself after joining.
#include "net.h"
#include "store.h"
#include "rtc.h"
#include "power.h"
#include "powerlog.h"
#include "weather.h"
#include "display.h"
#include <string.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "textfit.h"
#include "lwip/sockets.h"

static const char *TAG = "net";
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static esp_netif_t *s_sta, *s_ap;
static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0
#define EV_FAIL   BIT1
static volatile net_mode_t s_mode = NET_OFF;
static httpd_handle_t s_http;
static volatile int64_t s_activity;
static char s_ip[16];
static char s_ap_ssid[24];
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static bool s_wifi_inited, s_sntp_on;
// Quiet mode: a background weather check joins home Wi-Fi without waking the screen and
// without opening the hotspot if home Wi-Fi isn't reachable. Opening Tasks upgrades it to normal.
static volatile bool s_quiet, s_quiet_done, s_allow_ap = true;
static volatile bool s_scanning;   // looking for saved networks: don't auto-connect yet
static int s_retries;
static char s_joined[33];          // the saved network we're on
// Trying a network you just added while the phone is on the device's hotspot: the hotspot stays
// up and the device joins your network alongside it, then reports how it went.
static char s_test_ssid[33]; static volatile bool s_testing; static volatile int s_test_reason;
static volatile int s_last_reason;           // why the last join attempt dropped
// What the device last learned about each saved network (RAM only: no flash writes).
// Updated by the join test, the hourly forecast join and every scan.
// NS_UNCHECKED: in range, but the password couldn't be tried. On home Wi-Fi, Chibi can't join a
// second network without dropping the one your phone is using to reach it; it gets checked the
// next time Chibi joins it.
enum { NS_NONE, NS_TESTING, NS_OK, NS_WRONGPASS, NS_NOTNEAR, NS_FAILED, NS_UNCHECKED };
static const char *const NS_NAME[] = { "", "testing", "ok", "wrongpass", "notnear", "failed", "unchecked" };
static struct { char ssid[33]; uint8_t st; } s_ns[MAX_WIFI + 1];
static portMUX_TYPE s_ns_mux = portMUX_INITIALIZER_UNLOCKED;
static int ns_get(const char *ssid)
{
    int st = NS_NONE;
    taskENTER_CRITICAL(&s_ns_mux);
    for (int i = 0; i <= MAX_WIFI; i++) if (s_ns[i].ssid[0] && !strcmp(s_ns[i].ssid, ssid)) st = s_ns[i].st;
    taskEXIT_CRITICAL(&s_ns_mux);
    return st;
}
static void ns_set(const char *ssid, int st)
{
    static uint8_t rr;
    taskENTER_CRITICAL(&s_ns_mux);
    int k = -1;
    for (int i = 0; i <= MAX_WIFI && k < 0; i++) if (s_ns[i].ssid[0] && !strcmp(s_ns[i].ssid, ssid)) k = i;
    for (int i = 0; i <= MAX_WIFI && k < 0; i++) if (!s_ns[i].ssid[0]) k = i;
    if (k < 0) k = rr++ % (MAX_WIFI + 1);
    if (st == NS_NONE) s_ns[k].ssid[0] = 0;
    else { strlcpy(s_ns[k].ssid, ssid, sizeof s_ns[k].ssid); s_ns[k].st = st; }
    taskEXIT_CRITICAL(&s_ns_mux);
}
static int reason_to_ns(int r)
{
    if (r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) return NS_NOTNEAR;
    if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
        r == WIFI_REASON_MIC_FAILURE || r == WIFI_REASON_AUTH_EXPIRE) return NS_WRONGPASS;
    return NS_FAILED;
}
// One radio job at a time: a scan, a join test or the start-up join.
static volatile bool s_radio_busy;
static bool radio_claim(void)
{
    taskENTER_CRITICAL(&s_ns_mux);
    bool got = !s_radio_busy; s_radio_busy = true;
    taskEXIT_CRITICAL(&s_ns_mux);
    return got;
}
// The last scan, kept briefly so reopening the list doesn't scan again.
typedef struct { char ssid[33]; int8_t rssi; uint8_t open; } seen_t;
#define SEEN_MAX 16
static seen_t s_seen[SEEN_MAX]; static int s_nseen; static int64_t s_seen_at = -1;

net_mode_t net_mode(void) { return s_mode; }
static volatile bool s_sta_lost;
static volatile int64_t s_req_at = -1;      // last web request (for the status line)
int64_t net_last_activity_ms(void) { return s_activity; }
static void touch_activity(void) { s_activity = esp_timer_get_time() / 1000; }
// The open page checks /api/state every 3 s. That keeps Wi-Fi on, but on its own it shouldn't keep
// the screen from dimming: only real use of the page (changes, opening it, text size, scans) does.
static volatile int64_t s_user;
int64_t net_last_user_ms(void) { return s_user; }
static void touch_user(void) { s_user = esp_timer_get_time() / 1000; }

void net_qr_text(char *buf, int len)
{
    store_lock();
    if (s_mode == NET_STA) snprintf(buf, len, "http://%s/?t=%s", s_ip, store_net()->token);
    else snprintf(buf, len, "WIFI:T:WPA;S:%s;P:%s;;", s_ap_ssid, store_net()->ap_pass);
    store_unlock();
}
void net_line1(char *buf, int len)
{
    if (s_mode == NET_STA) snprintf(buf, len, "Scan to add tasks");
    else if (s_mode == NET_AP) snprintf(buf, len, "Scan to join, page opens");
    else snprintf(buf, len, "Starting Wi-Fi...");
}
void net_line2(char *buf, int len)
{
    if (s_mode == NET_STA) snprintf(buf, len, "chibi.local");
    else if (s_mode == NET_AP) snprintf(buf, len, "%s", s_ap_ssid);
    else buf[0] = 0;
}

// ------------------------------------------------------------------ events
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) { if (!s_scanning) { s_retries = 0; esp_wifi_connect(); } }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_last_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        if (s_scanning) return;
        if (s_testing) { s_test_reason = ((wifi_event_sta_disconnected_t *)data)->reason; xEventGroupSetBits(s_ev, EV_FAIL); return; }
        if (s_mode == NET_STA) {             // the router dropped us: join again (the address may change)
            ESP_LOGW(TAG, "lost home Wi-Fi (reason %d), joining again", s_last_reason);
            s_sta_lost = true;
            esp_wifi_connect();
            return;
        }
        if (s_mode == NET_STARTING && ++s_retries < 4) esp_wifi_connect();
        else if (s_mode == NET_STARTING) xEventGroupSetBits(s_ev, EV_FAIL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "a phone joined the hotspot (%02x:%02x:..:%02x, free RAM %u)", e->mac[0], e->mac[1], e->mac[5],
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "a phone left the hotspot (reason %d)", (int)((wifi_event_ap_stadisconnected_t *)data)->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ESP_LOGI(TAG, "gave the phone the address " IPSTR, IP2STR(&((ip_event_ap_staipassigned_t *)data)->ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        if (!s_testing) snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        if (s_sta_lost && !s_testing) { s_sta_lost = false; ESP_LOGI(TAG, "home Wi-Fi back, address %s", s_ip); }
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

void net_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_ev = xEventGroupCreate();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, NULL);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "Chibi-%02X%02X", mac[4], mac[5]);
}

// ------------------------------------------------------------------ captive DNS (hotspot mode only)
static void dns_task(void *arg)
{
    // Phones check for internet by looking up a test address (captive.apple.com,
    // connectivitycheck.gstatic.com, ...). Every name is answered with this device, so their check
    // lands on our web server, which sends it to the page: the phone then shows the sign-in page.
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    for (int i = 0; i < 20 && bind(sock, (struct sockaddr *)&a, sizeof a) != 0; i++) vTaskDelay(pdMS_TO_TICKS(100));   // the last run may still hold port 53
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static uint8_t buf[512];                                      // off the stack: this task's stack is small
    while (s_dns_run) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // never spin on an error
        if (n < 12 || (buf[2] & 0x80)) continue;                  // too short, or not a query
        int q = 12; while (q < n && buf[q] && buf[q] < 64) q += buf[q] + 1;
        if (q + 5 > n || buf[q]) continue;                        // malformed name
        int qtype = (buf[q + 1] << 8) | buf[q + 2];
        {   // log the names asked for (first 40 per start), so a phone that doesn't show the page can be diagnosed
            static int logged; if (logged++ < 40) {
                char nm[64]; int o = 0; for (int p = 12; p < q && buf[p] && o < 60; p += buf[p] + 1) { if (o) nm[o++] = '.'; for (int k = 1; k <= buf[p] && o < 62; k++) nm[o++] = buf[p + k]; }
                nm[o] = 0; ESP_LOGI(TAG, "DNS %s (type %d) -> %s", nm, qtype, qtype == 1 || qtype == 255 ? "192.168.4.1" : "no address");
            } }
        q += 5;                                                   // end of the (first) question
        buf[2] = 0x84 | (buf[2] & 0x01); buf[3] = 0x80;           // answer, authoritative, keep "recursion desired"
        buf[4] = 0; buf[5] = 1;                                   // one question
        buf[8] = buf[9] = buf[10] = buf[11] = 0;                  // no authority / extra records
        int len = q;
        if (qtype == 1 || qtype == 255) {                         // A (or ANY): 192.168.4.1
            uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + q, ans, 16); len += 16;
            buf[6] = 0; buf[7] = 1;
        } else { buf[6] = 0; buf[7] = 0; }                        // AAAA and others: no address, so phones use the A answer
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
}

// ------------------------------------------------------------------ HTTP
static bool authorized(httpd_req_t *r)
{
    touch_activity(); s_req_at = esp_timer_get_time();
    if (r->method != HTTP_GET || (strncmp(r->uri, "/api/state", 10) && strncmp(r->uri, "/api/diag", 9))) touch_user();
    if (s_mode == NET_AP) return true;        // joining the hotspot already needed the password from the screen
    char tok[16] = "";
    if (httpd_req_get_hdr_value_str(r, "X-Token", tok, sizeof tok) != ESP_OK) {
        char q[48];   // also accept ?t=TOKEN so /api/diag can be opened from the address bar
        if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK) httpd_query_key_value(q, "t", tok, sizeof tok);
    }
    store_lock(); bool ok = strcmp(tok, store_net()->token) == 0; store_unlock();
    if (!ok) { httpd_resp_set_status(r, "403 Forbidden"); httpd_resp_sendstr(r, "{\"error\":\"token\"}"); }
    return ok;
}

static char *read_body(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *b = malloc(len + 1);
    int got = 0;
    while (got < len) { int k = httpd_req_recv(r, b + got, len - got); if (k <= 0) { free(b); return NULL; } got += k; }
    b[len] = 0;
    return b;
}

static esp_err_t send_state(httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "tasks");
    store_lock();
    tasklist_t *tl = store_tasks();
    for (int i = 0; i < tl->n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id", tl->t[i].id);
        cJSON_AddStringToObject(t, "n", tl->t[i].name);
        cJSON_AddBoolToObject(t, "d", tl->t[i].done);
        cJSON_AddItemToArray(arr, t);
    }
    cJSON_AddNumberToObject(o, "studyLen", store_settings()->study_min);
    cJSON_AddNumberToObject(o, "taskFont", store_settings()->task_font);
    cJSON_AddNumberToObject(o, "breakLen", store_settings()->break_min);
    cJSON_AddNumberToObject(o, "sessions", store_session()->sessions);
    cJSON *wa = cJSON_AddArrayToObject(o, "wifis");
    for (int i = 0; i < store_wifis()->n; i++) {
        const char *id = store_wifis()->w[i].ssid;
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "ssid", id);
        cJSON_AddStringToObject(x, "st", NS_NAME[ns_get(id)]);
        cJSON_AddItemToArray(wa, x);
    }
    cJSON_AddStringToObject(o, "wifiNow", s_mode == NET_STA ? s_joined : "");
    cJSON *w = cJSON_AddObjectToObject(o, "weather");
    cJSON_AddBoolToObject(w, "on", store_wx()->on);
    cJSON_AddBoolToObject(w, "auto", store_wx()->autoloc);
    cJSON_AddStringToObject(w, "city", store_wx()->city);
    cJSON_AddStringToObject(w, "place", store_wx()->located ? store_wx()->place : "");
    store_unlock();
    char st[96]; weather_status(st, sizeof st);
    cJSON_AddStringToObject(w, "status", st);
    rain_info_t ri;
    if (weather_rain_today(&ri)) {
        cJSON_AddBoolToObject(w, "rain", ri.rain);
        if (ri.rain) { cJSON_AddNumberToObject(w, "fromHour", ri.from_hour); cJSON_AddBoolToObject(w, "now", ri.now); cJSON_AddBoolToObject(w, "snow", ri.snow); cJSON_AddNumberToObject(w, "chance", ri.max_prob); }
    }
    int pct = power_battery_pct();
    if (pct >= 0) cJSON_AddNumberToObject(o, "battery", pct); else cJSON_AddNullToObject(o, "battery");
    cJSON_AddBoolToObject(o, "charging", power_on_usb());
    cJSON_AddNumberToObject(o, "epoch", rtc_time_valid() ? (double)time(NULL) : 0);
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, s);
    free(s); cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_index(httpd_req_t *r)
{
    touch_activity(); touch_user(); s_req_at = esp_timer_get_time();
    ESP_LOGI(TAG, "sending the page");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

static esp_err_t h_state(httpd_req_t *r) { if (!authorized(r)) return ESP_OK; return send_state(r); }

static esp_err_t bad(httpd_req_t *r, const char *msg) { httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, msg); return ESP_OK; }

static esp_err_t h_add(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    const cJSON *n = cJSON_GetObjectItem(j, "n");
    bool ok = cJSON_IsString(n) && store_task_add(n->valuestring);
    cJSON_Delete(j);
    if (!ok) return bad(r, "{\"error\":\"could not add\"}");
    return send_state(r);
}

static uint16_t id_from_uri(httpd_req_t *r) { const char *p = strrchr(r->uri, '/'); return p ? (uint16_t)atoi(p + 1) : 0; }

static esp_err_t h_patch(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    uint16_t id = id_from_uri(r);
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    const cJSON *d = cJSON_GetObjectItem(j, "d"), *n = cJSON_GetObjectItem(j, "n");
    bool ok = true;
    if (cJSON_IsBool(d)) ok &= store_task_set_done(id, cJSON_IsTrue(d));
    if (cJSON_IsString(n)) ok &= store_task_rename(id, n->valuestring);
    cJSON_Delete(j);
    if (!ok) return bad(r, "{\"error\":\"no such task\"}");
    return send_state(r);
}

static esp_err_t h_delete(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    store_task_delete(id_from_uri(r));
    return send_state(r);
}

static esp_err_t h_order(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    const cJSON *ids = cJSON_GetObjectItem(j, "ids");
    uint16_t v[MAX_TASKS]; int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, ids) if (cJSON_IsNumber(it) && n < MAX_TASKS) v[n++] = (uint16_t)it->valueint;
    cJSON_Delete(j);
    store_task_reorder(v, n);
    return send_state(r);
}

static bool in_list(int v, const int *l, int n) { for (int i = 0; i < n; i++) if (l[i] == v) return true; return false; }

static esp_err_t h_settings(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    static const int STUDY[] = {15,20,25,30,35,40,45,50,60}, BRK[] = {3,5,10,15,20};
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    const cJSON *s = cJSON_GetObjectItem(j, "studyLen"), *k = cJSON_GetObjectItem(j, "breakLen");
    store_lock();
    if (cJSON_IsNumber(s) && in_list(s->valueint, STUDY, 9)) store_settings()->study_min = s->valueint;
    if (cJSON_IsNumber(k) && in_list(k->valueint, BRK, 5)) store_settings()->break_min = k->valueint;
    // task text size: while the slider is being dragged ("preview") the device shows it at once but
    // doesn't write to flash; the value is saved when the finger lets go
    const cJSON *tf = cJSON_GetObjectItem(j, "taskFont"), *pv = cJSON_GetObjectItem(j, "preview");
    if (cJSON_IsNumber(tf) && tf->valueint >= 0 && tf->valueint <= 3) store_settings()->task_font = tf->valueint;
    if (cJSON_IsTrue(pv)) store_touch(); else store_mark_settings();
    if (cJSON_IsNumber(tf)) touch_activity();
    store_unlock();
    cJSON_Delete(j);
    return send_state(r);
}

// Where each task name gets its "..." on the device, at all 4 text sizes, worked out with the
// device's own fonts so the phone's preview cuts at exactly the same letter.
// [[id, keep0, keep1, keep2, keep3], ...]; keep is -1 when the whole name fits.
static esp_err_t h_fit(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    task_t *t = malloc(sizeof(task_t) * MAX_TASKS);
    char *out = malloc(MAX_TASKS * 32 + 16);
    if (!t || !out) { free(t); free(out); httpd_resp_send_500(r); return ESP_OK; }
    store_lock(); int n = store_tasks()->n; memcpy(t, store_tasks()->t, sizeof(task_t) * n); store_unlock();
    int o = snprintf(out, 16, "{\"fit\":[");
    for (int i = 0; i < n; i++)
        o += sprintf(out + o, "%s[%u,%d,%d,%d,%d]", i ? "," : "", t[i].id,
                     textfit_keep(0, t[i].name), textfit_keep(1, t[i].name), textfit_keep(2, t[i].name), textfit_keep(3, t[i].name));
    strcpy(out + o, "]}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, out);
    free(t); free(out);
    return ESP_OK;
}

static esp_err_t h_time(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    const cJSON *e = cJSON_GetObjectItem(j, "epoch"), *tz = cJSON_GetObjectItem(j, "tzOffsetMin");
    if (cJSON_IsNumber(tz) && tz->valueint > -900 && tz->valueint < 900) {
        store_lock(); store_settings()->tz_min = tz->valueint; store_mark_settings(); store_unlock();
        rtc_set_tz_offset_min(tz->valueint);
    }
    if (cJSON_IsNumber(e) && e->valuedouble > 1700000000) rtc_set_epoch((time_t)e->valuedouble);
    cJSON_Delete(j);
    return send_state(r);
}

// Looks for nearby networks: about a second, only when the page asks. In hotspot mode the
// station side is switched on just for the scan, then off again.
static bool scan_now(void)
{
    if (s_mode != NET_AP && s_mode != NET_STA) return false;
    if (!radio_claim()) return false;
    bool ap = s_mode == NET_AP;
    s_scanning = true;                                   // no auto-connect while the station side is up
    if (ap) esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_scan_config_t sc = { .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = { .min = 30, .max = 80 } };
    bool ok = esp_wifi_scan_start(&sc, true) == ESP_OK;
    uint16_t n = 0;
    wifi_ap_record_t *rec = NULL;
    if (ok) { esp_wifi_scan_get_ap_num(&n); if (n > 40) n = 40; rec = n ? calloc(n, sizeof *rec) : NULL; }
    if (ok && rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        // records come strongest first; keep one entry per name
        seen_t tmp[SEEN_MAX]; int m = 0;
        for (int i = 0; i < n && m < SEEN_MAX; i++) {
            const char *id = (const char *)rec[i].ssid;
            if (!id[0] || !strcmp(id, s_ap_ssid)) continue;
            bool dup = false; for (int k = 0; k < m && !dup; k++) dup = !strcmp(tmp[k].ssid, id);
            if (dup) continue;
            strlcpy(tmp[m].ssid, id, sizeof tmp[m].ssid); tmp[m].rssi = rec[i].rssi; tmp[m].open = rec[i].authmode == WIFI_AUTH_OPEN; m++;
        }
        memcpy(s_seen, tmp, sizeof tmp); s_nseen = m; s_seen_at = esp_timer_get_time();
        // a saved network that was "not nearby" and now shows up is back to plain "saved"
        wifilist_t wl; store_lock(); wl = *store_wifis(); store_unlock();
        for (int k = 0; k < wl.n; k++) {
            bool here = false; for (int i = 0; i < m && !here; i++) here = !strcmp(tmp[i].ssid, wl.w[k].ssid);
            int st = ns_get(wl.w[k].ssid);
            if (here && st == NS_NOTNEAR) ns_set(wl.w[k].ssid, NS_UNCHECKED);
            else if (!here && (st == NS_NONE || st == NS_UNCHECKED) && strcmp(wl.w[k].ssid, s_joined)) ns_set(wl.w[k].ssid, NS_NOTNEAR);
        }
    } else ok = false;
    free(rec);
    if (!ok) esp_wifi_scan_stop();
    if (ap) esp_wifi_set_mode(WIFI_MODE_AP);
    s_scanning = false;
    s_radio_busy = false;
    return ok;
}

// Checks a network you just added or fixed. On the device's hotspot it joins it for real
// (the hotspot stays up). On home Wi-Fi it can't leave the network your phone is using,
// so it only checks that the network is nearby.
static void wifi_test_task(void *arg)
{
    wifi_config_t wc = { 0 };
    wifilist_t wl; store_lock(); wl = *store_wifis(); store_unlock();
    for (int k = 0; k < wl.n; k++) if (!strcmp(wl.w[k].ssid, s_test_ssid)) {
        snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", wl.w[k].ssid);
        snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", wl.w[k].pass);
    }
    for (int t = 0; t < 50 && s_radio_busy; t++) vTaskDelay(pdMS_TO_TICKS(100));   // a scan is finishing
    int result = NS_UNCHECKED;            // stays so if the radio is busy or Wi-Fi went off meanwhile
    if (s_mode == NET_STA) {
        result = NS_UNCHECKED;
        if (strcmp(s_test_ssid, s_joined) && scan_now()) {
            bool here = false; for (int i = 0; i < s_nseen && !here; i++) here = !strcmp(s_seen[i].ssid, s_test_ssid);
            result = here ? NS_UNCHECKED : NS_NOTNEAR;
        } else if (!strcmp(s_test_ssid, s_joined)) result = NS_OK;
    } else if (s_mode == NET_AP && radio_claim()) {
        result = NS_FAILED;
        for (int attempt = 0; attempt < 2 && result != NS_OK && result != NS_WRONGPASS && result != NS_NOTNEAR; attempt++) {
            xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
            s_testing = true; s_test_reason = 0;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_connect();
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
            result = (b & EV_GOT_IP) ? NS_OK : reason_to_ns(s_test_reason);
            esp_wifi_disconnect();
            s_testing = false;
        }
        esp_wifi_set_mode(WIFI_MODE_AP);    // back to the hotspot alone: the station side costs power
        s_radio_busy = false;
    }
    ESP_LOGI(TAG, "Wi-Fi check %s: %s (reason %d)", s_test_ssid, NS_NAME[result], s_test_reason);
    ns_set(s_test_ssid, result);
    s_test_ssid[0] = 0;
    touch_activity();
    vTaskDelete(NULL);
}

static esp_err_t h_wifi(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    // {ssid, pass} adds a network or changes its password; {ssid, remove:true} forgets it
    const cJSON *s = cJSON_GetObjectItem(j, "ssid"), *p = cJSON_GetObjectItem(j, "pass"), *rm = cJSON_GetObjectItem(j, "remove");
    if (cJSON_IsString(s) && s->valuestring[0] && strlen(s->valuestring) <= 32 && (!cJSON_IsString(p) || strlen(p->valuestring) <= 64)) {
        if (cJSON_IsTrue(rm)) { store_wifi_remove(s->valuestring); ns_set(s->valuestring, NS_NONE); }
        else if (store_wifi_set(s->valuestring, cJSON_IsString(p) ? p->valuestring : NULL) < 0) { cJSON_Delete(j); return bad(r, "Up to 3 networks. Remove one first."); }
        else if (!s_test_ssid[0]) {                       // saved either way; now check it
            snprintf(s_test_ssid, sizeof s_test_ssid, "%s", s->valuestring);
            ns_set(s_test_ssid, NS_TESTING);
            if (xTaskCreate(wifi_test_task, "wifitest", 4096, NULL, 4, NULL) != pdPASS) { ns_set(s_test_ssid, NS_NONE); s_test_ssid[0] = 0; }
        } else ns_set(s->valuestring, NS_UNCHECKED);   // another check is running
        weather_poke();
    }
    cJSON_Delete(j);
    return send_state(r);
}

// Nearby networks for the "Nearby" list: [{s, r, o}] strongest first (s name, r signal dBm,
// o open = no password). Reuses a scan from the last 15 s unless ?fresh=1 (then at most one
// scan every 4 s), so opening and closing the list doesn't keep the radio busy.
static esp_err_t h_scan(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char q[24] = ""; bool fresh = httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK && strstr(q, "fresh=1");
    int64_t age = s_seen_at < 0 ? INT64_MAX : esp_timer_get_time() - s_seen_at;
    bool ok = true;
    if (age > 15000000 || (fresh && age > 4000000)) ok = scan_now();
    if (!ok && s_seen_at < 0) { httpd_resp_set_status(r, "503 Busy"); httpd_resp_sendstr(r, "{\"busy\":true}"); return ESP_OK; }
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "nets");
    for (int i = 0; i < s_nseen; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "s", s_seen[i].ssid);
        cJSON_AddNumberToObject(x, "r", s_seen[i].rssi);
        if (s_seen[i].open) cJSON_AddTrueToObject(x, "o");
        cJSON_AddItemToArray(a, x);
    }
    char *txt = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, txt);
    free(txt); cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_weather(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *b = read_body(r); cJSON *j = b ? cJSON_Parse(b) : NULL; free(b);
    // {city:"Tokyo"} a named place · {auto:true} approximate, from the connection · {off:true}
    const cJSON *c = cJSON_GetObjectItem(j, "city"), *au = cJSON_GetObjectItem(j, "auto"), *off = cJSON_GetObjectItem(j, "off");
    store_lock();
    wxcfg_t *w = store_wx();
    if (cJSON_IsTrue(off)) { memset(w, 0, sizeof *w); store_mark_wx(); }
    else if (cJSON_IsTrue(au)) { if (!w->on || !w->autoloc) { memset(w, 0, sizeof *w); w->on = 1; w->autoloc = 1; store_mark_wx(); } }
    else if (cJSON_IsString(c) && c->valuestring[0]) {
        if (!w->on || w->autoloc || strcmp(w->city, c->valuestring)) {
            memset(w, 0, sizeof *w); w->on = 1;
            snprintf(w->city, sizeof w->city, "%s", c->valuestring);
            store_mark_wx();
        }
    }
    store_unlock();
    weather_poke();
    cJSON_Delete(j);
    return send_state(r);
}

static esp_err_t h_diag(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    char *buf = malloc(2600);
    if (!buf) { httpd_resp_send_500(r); return ESP_OK; }
    power_diag_json(buf, 1500);
    int n = strlen(buf);
    if (n > 1 && buf[n - 1] == '}') {           // add the frame-rate readout to the same JSON
        buf[n - 1] = 0;
        char d[256]; display_stats_json(d, sizeof d);
        char b[640]; powerlog_json(b, sizeof b);
        snprintf(buf + n - 1, 2600 - n, ",\"display\":%s,\"battery\":%s}", d, b);
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t h_404(httpd_req_t *r, httpd_err_code_t err)
{
    if (s_mode == NET_AP) {          // captive portal: send every unknown URL (the phone's internet check) to the page
        touch_activity();
        { char host[64] = ""; httpd_req_get_hdr_value_str(r, "Host", host, sizeof host);
          ESP_LOGI(TAG, "phone check http://%s%s -> sent to the page (%d)", host, r->uri, (int)err); }
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        httpd_resp_set_hdr(r, "Connection", "close");      // an iPhone opens many connections on joining: free each one at once
        httpd_resp_set_type(r, "text/html");
        httpd_resp_sendstr(r, "<!doctype html><meta http-equiv=refresh content=\"0;url=http://192.168.4.1/\"><a href=\"http://192.168.4.1/\">Open</a>");
        return ESP_OK;
    }
    httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

static void http_start(void)
{
    if (s_http) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_uri_handlers = 16;
    c.stack_size = 8192;
    // The stack goes in PSRAM. After hours on home Wi-Fi the internal RAM was too broken up for an
    // 8 KB block, and the page server couldn't start. Its handlers never write to flash (saving
    // happens in the UI task), which is what a PSRAM stack must not do.
    c.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    c.lru_purge_enable = true;
    c.max_open_sockets = 10;          // phones open a burst of connections right after joining
    esp_err_t err = httpd_start(&s_http, &c);
    if (err != ESP_OK) { ESP_LOGE(TAG, "web page server failed to start: %s", esp_err_to_name(err)); s_http = NULL; return; }
    const httpd_uri_t u[] = {
        { "/", HTTP_GET, h_index }, { "/index.html", HTTP_GET, h_index },
        { "/api/state", HTTP_GET, h_state }, { "/api/diag", HTTP_GET, h_diag }, { "/api/fit", HTTP_GET, h_fit }, { "/api/scan", HTTP_GET, h_scan },
        { "/api/tasks", HTTP_POST, h_add }, { "/api/tasks/*", HTTP_PATCH, h_patch }, { "/api/tasks/*", HTTP_DELETE, h_delete },
        { "/api/order", HTTP_POST, h_order }, { "/api/settings", HTTP_POST, h_settings },
        { "/api/time", HTTP_POST, h_time }, { "/api/wifi", HTTP_POST, h_wifi }, { "/api/weather", HTTP_POST, h_weather },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(s_http, &u[i]);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_404);
    httpd_register_err_handler(s_http, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, h_404);   // big probe headers: still send it to the page
}

// ------------------------------------------------------------------ start / stop
static void sntp_synced(struct timeval *tv) { rtc_set_epoch(tv->tv_sec); ESP_LOGI(TAG, "clock set from the internet"); }

static void start_task(void *arg)
{
    netcfg_t nc;
    store_lock(); nc = *store_net(); store_unlock();
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_wifi_inited = true;
    }
    bool sta_ok = false;
    wifilist_t wl; store_lock(); wl = *store_wifis(); store_unlock();
    s_joined[0] = 0;
    if (wl.n) {
        // scan, then join the strongest saved network that's in range
        s_scanning = true;
        wifi_config_t none = { 0 };
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &none);
        esp_wifi_start();
        int pick = -1, best = -1000;
        wifi_scan_config_t sc = { 0 };
        if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
            uint16_t n = 24;
            wifi_ap_record_t *rec = calloc(n, sizeof *rec);
            if (rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
                for (int i = 0; i < n; i++) for (int k = 0; k < wl.n; k++)
                    if (!strcmp((char *)rec[i].ssid, wl.w[k].ssid) && rec[i].rssi > best) { best = rec[i].rssi; pick = k; }
                for (int k = 0; k < wl.n; k++) {          // remember which saved networks were around
                    bool here = false; for (int i = 0; i < n && !here; i++) here = !strcmp((char *)rec[i].ssid, wl.w[k].ssid);
                    int st = ns_get(wl.w[k].ssid);
                    if (!here && st != NS_WRONGPASS) ns_set(wl.w[k].ssid, NS_NOTNEAR);
                    else if (here && st == NS_NOTNEAR) ns_set(wl.w[k].ssid, NS_UNCHECKED);
                }
            }
            free(rec);
        }
        if (pick < 0 && wl.n == 1) pick = 0;      // a hidden network doesn't show in scans: try it anyway
        s_scanning = false;
        if (pick >= 0) {
            xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
            wifi_config_t wc = { 0 };
            snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", wl.w[pick].ssid);
            snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", wl.w[pick].pass);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            s_retries = 0;
            esp_wifi_connect();
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
            sta_ok = b & EV_GOT_IP;
            if (sta_ok) snprintf(s_joined, sizeof s_joined, "%s", wl.w[pick].ssid);
            ns_set(wl.w[pick].ssid, sta_ok ? NS_OK : reason_to_ns(s_last_reason));
        }
        if (!sta_ok) { ESP_LOGW(TAG, "no saved Wi-Fi reachable"); esp_wifi_disconnect(); esp_wifi_stop(); }
    }
    if (!sta_ok && !s_allow_ap) {          // quiet start: no hotspot
        s_mode = NET_OFF;
        xEventGroupSetBits(s_ev, EV_FAIL);
        vTaskDelete(NULL);
        return;
    }
    if (sta_ok) {
        mdns_init();
        mdns_hostname_set("chibi");
        mdns_instance_name_set("Chibi");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sc.sync_cb = sntp_synced;
        if (esp_netif_sntp_init(&sc) == ESP_OK) s_sntp_on = true;
        http_start();
        s_mode = NET_STA;
    } else {
        wifi_config_t ac = { 0 };
        snprintf((char *)ac.ap.ssid, sizeof ac.ap.ssid, "%s", s_ap_ssid);
        ac.ap.ssid_len = strlen(s_ap_ssid);
        snprintf((char *)ac.ap.password, sizeof ac.ap.password, "%s", nc.ap_pass);
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ac.ap.max_connection = 2;
        ac.ap.channel = 6;
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ac);
        esp_wifi_start();
        for (int i = 0; i < 30 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(100));   // the previous one is closing
        s_dns_run = true;
        if (!s_dns_task) xTaskCreateWithCaps(dns_task, "dns", 4096, NULL, 4, &s_dns_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // stack in PSRAM: see http_start
        http_start();
        snprintf(s_ip, sizeof s_ip, "192.168.4.1");
        s_mode = NET_AP;
    }
    if (!s_quiet) touch_activity();
    ESP_LOGI(TAG, "network up: %s %s (web page %s, free RAM %u)", s_mode == NET_STA ? "home Wi-Fi" : "hotspot", s_ip,
             s_http ? "ready" : "FAILED TO START", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    vTaskDelete(NULL);
}

void net_start(void)
{
    if (s_quiet) { s_quiet = false; s_allow_ap = true; }   // Tasks opened during a background check
    if (s_mode == NET_STARTING) return;
    if (s_mode != NET_OFF) { touch_activity(); return; }
    s_mode = NET_STARTING;
    s_allow_ap = true;
    touch_activity();
    xTaskCreate(start_task, "netstart", 6144, NULL, 4, NULL);
}

bool net_sta_acquire(int timeout_ms)
{
    if (s_mode == NET_STA) return true;
    if (s_mode != NET_OFF) return false;      // hotspot running (or starting): no internet
    int n; store_lock(); n = store_wifis()->n; store_unlock();
    if (!n) return false;
    s_quiet = true; s_quiet_done = false; s_allow_ap = false;
    s_mode = NET_STARTING;
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
    xTaskCreate(start_task, "netstart", 6144, NULL, 4, NULL);
    for (int t = 0; t < timeout_ms && s_mode == NET_STARTING; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_mode != NET_STA) { s_quiet = false; return false; }
    // wait a moment for the clock to sync if it was never set
    for (int t = 0; t < 5000 && !rtc_time_valid(); t += 100) vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

void net_log_status(void)
{
    if (s_mode == NET_OFF || s_mode == NET_STARTING) return;
    char link[48] = "";
    if (s_mode == NET_STA) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) snprintf(link, sizeof link, "connected to %s (%d dBm)", (char *)ap.ssid, ap.rssi);
        else snprintf(link, sizeof link, "NOT connected to the router");
    } else snprintf(link, sizeof link, "%s", s_ap_ssid);
    ESP_LOGI(TAG, "Wi-Fi: %s %s, %s, web page %s, last request %llds ago, free RAM %u (largest %u)",
             s_mode == NET_STA ? "home" : "hotspot", s_ip, link, s_http ? "on" : "OFF",
             s_req_at < 0 ? -1LL : (long long)((esp_timer_get_time() - s_req_at) / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void net_sta_release(void) { if (s_quiet) s_quiet_done = true; }

bool net_quiet_finished(void) { return s_quiet && s_quiet_done; }

void net_stop(void)
{
    if (s_mode == NET_OFF || s_mode == NET_STARTING) return;
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    s_dns_run = false;
    if (s_sntp_on) { esp_netif_sntp_deinit(); s_sntp_on = false; }
    mdns_free();
    esp_wifi_stop();
    s_quiet = false; s_quiet_done = false; s_allow_ap = true;
    s_mode = NET_OFF;
    s_seen_at = -1; s_nseen = 0;
    ESP_LOGI(TAG, "network off");
}
