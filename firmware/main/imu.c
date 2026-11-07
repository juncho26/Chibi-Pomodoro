// QMI8658 accelerometer: orientation with hysteresis, the ●● ● knock pattern, and wake-on-motion.
#include "imu.h"
#include "board.h"
#include "power.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_dev;
static QueueHandle_t s_q;
static volatile orient_t s_cur = ORI_NONE;
static volatile bool s_flat;
static volatile int s_map = 0;
static float s_g[3] = {0, 0, 1};
static float s_ref[4][3];          // calibrated gravity per position (unit vectors)
static volatile bool s_cal_ok;

#define R_WHOAMI 0x00
#define R_CTRL1  0x02
#define R_CTRL2  0x03
#define R_CTRL7  0x08
#define R_CTRL9  0x0A
#define R_CAL1_L 0x0B
#define R_CAL1_H 0x0C
#define R_STATUSINT 0x2D
#define R_AX_L   0x35
#define R_RESET  0x60

static esp_err_t wr(uint8_t r, uint8_t v) { uint8_t b[2] = { r, v }; return i2c_master_transmit(s_dev, b, 2, 50); }
static esp_err_t rd(uint8_t r, uint8_t *v, size_t n) { return i2c_master_transmit_receive(s_dev, &r, 1, v, n, 50); }

static void accel_config_normal(void)
{
    wr(R_CTRL7, 0x00);
    wr(R_CTRL1, 0x40);             // address auto-increment, little endian, INT pins disabled
    wr(R_CTRL2, (1 << 4) | 6);     // +-4 g, 125 Hz
    wr(R_CTRL7, 0x01);             // accelerometer only
}

bool imu_init(void)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR_QMI8658, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(power_i2c_bus(), &dc, &s_dev) != ESP_OK) return false;
    uint8_t id = 0;
    if (rd(R_WHOAMI, &id, 1) != ESP_OK || id != 0x05) { ESP_LOGE(TAG, "QMI8658 not found (id=0x%02x)", id); return false; }
    wr(R_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(30));
    accel_config_normal();
    s_q = xQueueCreate(16, sizeof(imu_event_t));
    return true;
}

void imu_set_mapping(int m) { s_map = m & 7; s_cur = ORI_NONE; }
int  imu_mapping(void) { return s_map; }
orient_t imu_current(void) { return s_cur; }
bool imu_is_flat(void) { return s_flat; }
void imu_get_gravity(float g[3]) { g[0] = s_g[0]; g[1] = s_g[1]; g[2] = s_g[2]; }
bool imu_get_event(imu_event_t *ev) { return s_q && xQueueReceive(s_q, ev, 0) == pdTRUE; }

static void emit(imu_ev_type_t t, orient_t o) { imu_event_t e = { t, o }; if (s_q) xQueueSend(s_q, &e, 0); }

void imu_set_calibration(const float ref[4][3], bool valid)
{
    if (valid) for (int k = 0; k < 4; k++) {
        float n = sqrtf(ref[k][0] * ref[k][0] + ref[k][1] * ref[k][1] + ref[k][2] * ref[k][2]);
        for (int i = 0; i < 3; i++) s_ref[k][i] = n > 0.1f ? ref[k][i] / n : 0;
    }
    s_cal_ok = valid;
    s_cur = ORI_NONE;
}


static orient_t classify(const float g[3], bool *flat)
{
    if (s_cal_ok) {
        // nearest calibrated position; in between (or lying flat) keeps the current mode
        float n = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (n < 0.5f) { *flat = false; return ORI_NONE; }
        float best = -2, second = -2; int bk = -1;
        for (int k = 0; k < 4; k++) {
            float d = (g[0] * s_ref[k][0] + g[1] * s_ref[k][1] + g[2] * s_ref[k][2]) / n;
            if (d > best) { second = best; best = d; bk = k; } else if (d > second) second = d;
        }
        *flat = best < 0.5f;
        if (best > 0.82f && best - second > 0.3f) return (orient_t)bk;
        return ORI_NONE;
    }
    float lng = (s_map & 1) ? g[1] : g[0];
    float sht = (s_map & 1) ? g[0] : g[1];
    if (s_map & 2) lng = -lng;
    if (s_map & 4) sht = -sht;
    *flat = fabsf(g[2]) > 0.82f;
    if (*flat) return ORI_NONE;
    float al = fabsf(lng), as = fabsf(sht);
    if (al > 0.6f && al > as * 1.25f) return lng > 0 ? ORI_P : ORI_PF;
    if (as > 0.6f && as > al * 1.25f) return sht > 0 ? ORI_L : ORI_LF;
    return ORI_NONE;   // in between: keep the current mode
}

#define KNOCK_G 0.25f      // a knock on the desk next to it: ~0.3-1 g at the sensor (measured on a V2 board)
#define KNOCK_SHARP_MS 120 // a knock is over this fast; turning the device by hand keeps pushing for much longer
#define TURN_QUIET_MS 1500 // no knocks for this long after the device was turned
static void imu_task(void *arg)
{
    float slow[3] = {0, 0, 1}, prev[3] = {0, 0, 1};
    orient_t cand = ORI_NONE; int64_t cand_since = 0, turned_at = -100000;
    int64_t knocks[6]; int nk = 0; int64_t last_spike = 0, last_motion_ev = 0;
    float kg[3] = {0, 0, 1};                 // which way was down at the first knock
    int64_t pend_t = 0; float pend_d = 0, pg[3] = {0, 0, 1};   // a spike waiting to prove it was short, and which way was down then
    bool first = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t b[6];
        if (rd(R_AX_L, b, 6) != ESP_OK) continue;
        float a[3];
        for (int i = 0; i < 3; i++) a[i] = (int16_t)(b[2*i] | (b[2*i+1] << 8)) * (4.0f / 32768.0f);
        if (first) { for (int i = 0; i < 3; i++) { s_g[i] = slow[i] = prev[i] = a[i]; } first = false; }
        float jerk = sqrtf((a[0]-prev[0])*(a[0]-prev[0]) + (a[1]-prev[1])*(a[1]-prev[1]) + (a[2]-prev[2])*(a[2]-prev[2]));
        for (int i = 0; i < 3; i++) { prev[i] = a[i]; s_g[i] += (a[i] - s_g[i]) * 0.15f; slow[i] += (a[i] - slow[i]) * 0.04f; }
        int64_t now = esp_timer_get_time() / 1000;

        // ---- orientation (stable for 450 ms) ----
        bool flat;
        orient_t o = classify(s_g, &flat);
        s_flat = flat;
        if (o != cand) { cand = o; cand_since = now; }
        else if (o != ORI_NONE && o != s_cur && now - cand_since > 450) { s_cur = o; turned_at = now; nk = 0; pend_t = 0; emit(IMU_EV_ORIENT, o); }   // turned: forget any knocks

        // ---- knock detection: short spikes on top of gravity ----
        float dx = a[0] - slow[0], dy = a[1] - slow[1], dz = a[2] - slow[2];
        float d = sqrtf(dx*dx + dy*dy + dz*dz);
        float gm = sqrtf(slow[0]*slow[0] + slow[1]*slow[1] + slow[2]*slow[2]);
        bool steady = fabsf(gm - 1.0f) < 0.2f && now - cand_since > 300 && now - turned_at > TURN_QUIET_MS;
        if (d > 0.08f && now - last_motion_ev > 1000) { last_motion_ev = now; emit(IMU_EV_MOTION, ORI_NONE); }
        // A knock is a short, sharp, moderate tap. Very hard hits (dropping it, banging it on
        // something) don't count; neither does the slow push of turning or flipping it over (it
        // doesn't start sharply and doesn't stop within 120 ms). The pattern must start after a quiet
        // moment and the device must not tilt more than ~20 degrees in the middle of it.
        static int64_t last_jolt;
        if (pend_t) {                                           // the last spike: over quickly enough?
            if (d < KNOCK_G * .6f) {
                int64_t t = pend_t; pend_t = 0;
                float gn = sqrtf(pg[0]*pg[0] + pg[1]*pg[1] + pg[2]*pg[2]);
                float same = gn > .5f ? (pg[0]*kg[0] + pg[1]*kg[1] + pg[2]*kg[2]) / gn : 0;
                if (nk && same < .94f) { ESP_LOGI(TAG, "knock ignored: the device moved (%.2f)", same); nk = 0; }
                else {
                    if (!nk) for (int i = 0; i < 3; i++) kg[i] = gn > .5f ? pg[i] / gn : 0;
                    ESP_LOGI(TAG, "knock %d (%.2f g)", nk + 1, pend_d);
                    if (nk < 6) knocks[nk++] = t;
                }
            } else if (now - pend_t > KNOCK_SHARP_MS) { ESP_LOGI(TAG, "not a knock: a push, not a tap"); pend_t = 0; nk = 0; last_jolt = now; }
        }
        bool spike = !pend_t && d > KNOCK_G && jerk > KNOCK_G * .6f && steady && now - last_spike > 70;
        if (spike && d > 2.5f) { nk = 0; spike = false; ESP_LOGI(TAG, "hit too hard for a knock (%.1f g)", d); }
        if (spike && (!nk || now - knocks[nk - 1] > 1300)) {
            nk = 0;
            if (now - last_jolt > 40 && now - last_jolt < 600) spike = false;   // it was already moving: not a fresh start
        }
        if (d > KNOCK_G * .7f && !pend_t && !spike) last_jolt = now;
        if (spike) { last_spike = now; pend_t = now; pend_d = d; for (int i = 0; i < 3; i++) pg[i] = slow[i]; }
        if (nk && !pend_t && now - knocks[nk - 1] > 650) {
            if (nk == 3) {
                int64_t g1 = knocks[1] - knocks[0], g2 = knocks[2] - knocks[1];
                if (g1 >= 70 && g1 <= 450 && g2 >= 450 && g2 <= 1300) emit(IMU_EV_KNOCK3, ORI_NONE);
                else ESP_LOGI(TAG, "knock rhythm not matched (%lld ms, %lld ms)", g1, g2);
            }
            nk = 0;
        }
    }
}

void imu_start(void)
{
    if (s_dev) xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 6, NULL, 1);
}

// Wake-on-motion, following the QMI8658 datasheet sequence (same as SensorLib::configWakeOnMotion).
void imu_arm_wake_on_motion(void)
{
    if (!s_dev) return;
    wr(R_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(30));
    wr(R_CTRL1, 0x40);
    wr(R_CTRL7, 0x00);
    wr(R_CTRL2, (2 << 4) | 12);            // +-8 g, low-power 128 Hz
    wr(R_CAL1_L, 120);                     // threshold: 120 mg
    wr(R_CAL1_H, (0x02 << 6) | 0x20);      // INT1, initial level high, blanking 32 samples
    wr(R_CTRL9, 0x08);                     // CTRL_CMD_WRITE_WOM_SETTING
    uint8_t st = 0;
    for (int i = 0; i < 200; i++) { rd(R_STATUSINT, &st, 1); if (st & 0x80) break; vTaskDelay(1); }
    wr(R_CTRL9, 0x00);                     // ack
    for (int i = 0; i < 200; i++) { rd(R_STATUSINT, &st, 1); if (!(st & 0x80)) break; vTaskDelay(1); }
    wr(R_CTRL7, 0x01);
    uint8_t c1 = 0; rd(R_CTRL1, &c1, 1); wr(R_CTRL1, c1 | (1 << 3));   // enable INT1
}
