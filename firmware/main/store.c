// Everything that must survive sleep, resets and power loss lives in NVS.
#include "store.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_log.h"

static SemaphoreHandle_t s_lock;
static tasklist_t s_tasks;
static settings_t s_set = { .study_min = 25, .break_min = 5, .imu_map = 0, .brightness = 220, .tz_min = 540 };
static session_t s_ses = { .study_left_ms = -1, .sessions_yday = -1 };
static netcfg_t s_net;
static wxcfg_t s_wx;
static wifilist_t s_wifis;
static imucal_t s_cal;
static volatile uint32_t s_ver = 1;
static volatile uint8_t s_dirty;   // bit0 tasks, bit1 settings, bit2 session, bit3 net, bit4 weather

void store_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void store_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
tasklist_t *store_tasks(void) { return &s_tasks; }
settings_t *store_settings(void) { return &s_set; }
session_t  *store_session(void) { return &s_ses; }
netcfg_t   *store_net(void) { return &s_net; }
wxcfg_t    *store_wx(void) { return &s_wx; }
wifilist_t *store_wifis(void) { return &s_wifis; }
imucal_t   *store_cal(void) { return &s_cal; }
uint32_t store_version(void) { return s_ver; }
void store_mark_tasks(void)    { s_dirty |= 1; s_ver++; }
void store_mark_settings(void) { s_dirty |= 2; s_ver++; }
void store_touch(void) { s_ver++; }
void store_mark_session(void)  { s_dirty |= 4; }
void store_mark_net(void)      { s_dirty |= 8; s_ver++; }
void store_mark_wx(void)       { s_dirty |= 16; s_ver++; }
void store_mark_wifis(void)    { s_dirty |= 32; s_ver++; }
void store_mark_cal(void)      { s_dirty |= 64; }

static void load_blob(nvs_handle_t h, const char *k, void *dst, size_t sz)
{
    size_t got = sz;
    void *tmp = malloc(sz);
    // a blob saved by an older version can be shorter (fields were added at the end): keep what it
    // has and leave the new fields at their defaults
    if (tmp && nvs_get_blob(h, k, tmp, &got) == ESP_OK && got <= sz) memcpy(dst, tmp, got);
    else if (tmp) { got = 0; if (nvs_get_blob(h, k, NULL, &got) == ESP_OK && got > sz) ESP_LOGW("store", "%s: saved by a newer version, ignored", k); }
    free(tmp);
}

static void rand_str(char *dst, int n, const char *alphabet)
{
    int an = strlen(alphabet);
    for (int i = 0; i < n; i++) dst[i] = alphabet[esp_random() % an];
    dst[n] = 0;
}

void store_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    nvs_handle_t h;
    if (nvs_open("comp", NVS_READWRITE, &h) == ESP_OK) {
        load_blob(h, "tasks", &s_tasks, sizeof s_tasks);
        load_blob(h, "set", &s_set, sizeof s_set);
        load_blob(h, "ses", &s_ses, sizeof s_ses);
        load_blob(h, "net", &s_net, sizeof s_net);
        load_blob(h, "wx", &s_wx, sizeof s_wx);
        load_blob(h, "wifis", &s_wifis, sizeof s_wifis);
        load_blob(h, "cal", &s_cal, sizeof s_cal);
        nvs_close(h);
    }
    if (s_tasks.n > MAX_TASKS) memset(&s_tasks, 0, sizeof s_tasks);
    if (!s_tasks.next_id) {
        // first boot: a few starter tasks so the screens aren't empty
        s_tasks.next_id = 1;
        store_task_add("Scan the QR to add tasks");
        store_task_add("Flip me over to study");
    }
    if (s_wifis.n > MAX_WIFI) memset(&s_wifis, 0, sizeof s_wifis);
    if (!s_wifis.n && s_net.ssid[0]) {      // one network saved by an older version
        snprintf(s_wifis.w[0].ssid, sizeof s_wifis.w[0].ssid, "%s", s_net.ssid);
        snprintf(s_wifis.w[0].pass, sizeof s_wifis.w[0].pass, "%s", s_net.pass);
        s_wifis.n = 1; s_net.ssid[0] = 0; s_net.pass[0] = 0; s_dirty |= 32 | 8;
    }
    if (!s_net.ap_pass[0]) { rand_str(s_net.ap_pass, 8, "abcdefghjkmnpqrstuvwxyz23456789"); s_dirty |= 8; }
    if (!s_net.token[0])   { rand_str(s_net.token, 8, "0123456789ABCDEF"); s_dirty |= 8; }
    store_flush();
}

void store_flush(void)
{
    if (!s_dirty) return;
    store_lock();
    uint8_t d = s_dirty; s_dirty = 0;
    tasklist_t *tl = NULL; settings_t st; session_t ss; netcfg_t nc; wxcfg_t wx; wifilist_t wl; imucal_t cal;
    if (d & 1) { tl = malloc(sizeof *tl); if (tl) *tl = s_tasks; }
    st = s_set; ss = s_ses; nc = s_net; wx = s_wx; wl = s_wifis; cal = s_cal;
    store_unlock();
    nvs_handle_t h;
    if (nvs_open("comp", NVS_READWRITE, &h) != ESP_OK) { free(tl); return; }
    if (tl) nvs_set_blob(h, "tasks", tl, sizeof *tl);
    if (d & 2) nvs_set_blob(h, "set", &st, sizeof st);
    if (d & 4) nvs_set_blob(h, "ses", &ss, sizeof ss);
    if (d & 8) nvs_set_blob(h, "net", &nc, sizeof nc);
    if (d & 16) nvs_set_blob(h, "wx", &wx, sizeof wx);
    if (d & 32) nvs_set_blob(h, "wifis", &wl, sizeof wl);
    if (d & 64) nvs_set_blob(h, "cal", &cal, sizeof cal);
    nvs_commit(h);
    nvs_close(h);
    free(tl);
}

static int find(uint16_t id) { for (int i = 0; i < s_tasks.n; i++) if (s_tasks.t[i].id == id) return i; return -1; }

static void copy_name(char *dst, const char *src)
{
    // cut at 40 characters (UTF-8 aware) and at the buffer size
    int chars = 0, i = 0;
    while (src[i] && chars < 40) {
        int len = ((unsigned char)src[i] < 0x80) ? 1 : ((unsigned char)src[i] >> 5) == 6 ? 2 : ((unsigned char)src[i] >> 4) == 14 ? 3 : 4;
        if (i + len >= TASK_NAME_MAX) break;
        i += len; chars++;
    }
    memcpy(dst, src, i); dst[i] = 0;
    for (char *p = dst; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
}

bool store_task_add(const char *name)
{
    store_lock();
    bool ok = s_tasks.n < MAX_TASKS && name && name[0];
    if (ok) {
        // new tasks go after the last unfinished one, above the done ones
        int pos = 0; while (pos < s_tasks.n && !s_tasks.t[pos].done) pos++;
        memmove(&s_tasks.t[pos + 1], &s_tasks.t[pos], (s_tasks.n - pos) * sizeof(task_t));
        task_t *t = &s_tasks.t[pos];
        memset(t, 0, sizeof *t);
        t->id = s_tasks.next_id++;
        copy_name(t->name, name);
        s_tasks.n++;
        store_mark_tasks();
    }
    store_unlock();
    return ok;
}

bool store_task_set_done(uint16_t id, bool done)
{
    store_lock();
    int i = find(id);
    if (i >= 0 && s_tasks.t[i].done != done) {
        task_t t = s_tasks.t[i];
        t.done = done;
        memmove(&s_tasks.t[i], &s_tasks.t[i + 1], (s_tasks.n - i - 1) * sizeof(task_t));
        s_tasks.n--;
        // done -> first slot of the done group (top of the bottom section); undone -> end of the to-do group
        int pos = 0; while (pos < s_tasks.n && !s_tasks.t[pos].done) pos++;
        memmove(&s_tasks.t[pos + 1], &s_tasks.t[pos], (s_tasks.n - pos) * sizeof(task_t));
        s_tasks.t[pos] = t; s_tasks.n++;
        store_mark_tasks();
    }
    store_unlock();
    return i >= 0;
}

bool store_task_rename(uint16_t id, const char *name)
{
    store_lock();
    int i = find(id);
    if (i >= 0 && name && name[0]) { copy_name(s_tasks.t[i].name, name); store_mark_tasks(); }
    store_unlock();
    return i >= 0;
}

bool store_task_delete(uint16_t id)
{
    store_lock();
    int i = find(id);
    if (i >= 0) { memmove(&s_tasks.t[i], &s_tasks.t[i + 1], (s_tasks.n - i - 1) * sizeof(task_t)); s_tasks.n--; store_mark_tasks(); }
    store_unlock();
    return i >= 0;
}

int store_task_clear_done(void)
{
    store_lock();
    int k = 0, removed = 0;
    for (int i = 0; i < s_tasks.n; i++) { if (s_tasks.t[i].done) removed++; else s_tasks.t[k++] = s_tasks.t[i]; }
    s_tasks.n = k;
    if (removed) store_mark_tasks();
    store_unlock();
    return removed;
}

void store_task_reorder(const uint16_t *ids, int n)
{
    store_lock();
    tasklist_t nt = { .n = 0, .next_id = s_tasks.next_id };
    for (int k = 0; k < n; k++) { int i = find(ids[k]); if (i >= 0 && nt.n < MAX_TASKS) { bool dup = false; for (int j = 0; j < nt.n; j++) if (nt.t[j].id == ids[k]) dup = true; if (!dup) nt.t[nt.n++] = s_tasks.t[i]; } }
    for (int i = 0; i < s_tasks.n; i++) { bool in = false; for (int j = 0; j < nt.n; j++) if (nt.t[j].id == s_tasks.t[i].id) in = true; if (!in && nt.n < MAX_TASKS) nt.t[nt.n++] = s_tasks.t[i]; }
    // keep the rule: unfinished first
    tasklist_t sorted = { .n = 0, .next_id = nt.next_id };
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nt.n; i++) if (nt.t[i].done == pass) sorted.t[sorted.n++] = nt.t[i];
    s_tasks = sorted;
    store_mark_tasks();
    store_unlock();
}

int store_wifi_set(const char *ssid, const char *pass)
{
    store_lock();
    int i = 0;
    while (i < s_wifis.n && strcmp(s_wifis.w[i].ssid, ssid)) i++;
    int r = 0;
    if (i == s_wifis.n) {
        if (s_wifis.n >= MAX_WIFI) r = -1;
        else { memset(&s_wifis.w[i], 0, sizeof s_wifis.w[i]); snprintf(s_wifis.w[i].ssid, sizeof s_wifis.w[i].ssid, "%s", ssid); s_wifis.n++; }
    }
    if (!r && pass) snprintf(s_wifis.w[i].pass, sizeof s_wifis.w[i].pass, "%s", pass);
    if (!r) store_mark_wifis();
    store_unlock();
    return r;
}

bool store_wifi_remove(const char *ssid)
{
    store_lock();
    bool found = false;
    for (int i = 0; i < s_wifis.n; i++) if (!strcmp(s_wifis.w[i].ssid, ssid)) {
        memmove(&s_wifis.w[i], &s_wifis.w[i + 1], (s_wifis.n - i - 1) * sizeof(wifi_t));
        s_wifis.n--; memset(&s_wifis.w[s_wifis.n], 0, sizeof(wifi_t)); found = true; break;
    }
    if (found) {
        store_mark_wifis();
    }
    store_unlock();
    return found;
}
