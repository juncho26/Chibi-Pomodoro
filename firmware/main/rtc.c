// PCF85063 real-time clock. Kept in UTC; the timezone lives in NVS and in the TZ variable.
#include "rtc.h"
#include "board.h"
#include "power.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_valid;

static uint8_t bcd2(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t tobcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void rtc_set_tz_offset_min(int off)
{
    // POSIX TZ sign is inverted: UTC+9 -> "UTC-9"
    char tz[24];
    int a = abs(off);
    snprintf(tz, sizeof tz, "LOC%c%d:%02d", off >= 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
}

bool rtc_init_and_load(void)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR_PCF85063, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(power_i2c_bus(), &dc, &s_dev) != ESP_OK) return false;
    uint8_t reg = 0x04, b[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 50) != ESP_OK) { ESP_LOGE(TAG, "RTC not responding"); return false; }
    if (b[0] & 0x80) { ESP_LOGW(TAG, "RTC lost power; time not set"); return false; }   // OS flag
    struct tm t = { .tm_sec = bcd2(b[0] & 0x7F), .tm_min = bcd2(b[1] & 0x7F), .tm_hour = bcd2(b[2] & 0x3F),
                    .tm_mday = bcd2(b[3] & 0x3F), .tm_mon = bcd2(b[5] & 0x1F) - 1, .tm_year = bcd2(b[6]) + 100 };
    if (t.tm_year < 124) return false;
    // interpret as UTC
    char *old = getenv("TZ"); char saved[32] = ""; if (old) snprintf(saved, sizeof saved, "%s", old);
    setenv("TZ", "UTC0", 1); tzset();
    time_t epoch = mktime(&t);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, NULL);
    s_valid = true;
    ESP_LOGI(TAG, "time loaded from RTC: %lld", (long long)epoch);
    return true;
}

void rtc_set_epoch(time_t epoch)
{
    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, NULL);
    s_valid = true;
    if (!s_dev) return;
    struct tm t; gmtime_r(&epoch, &t);
    uint8_t b[8] = { 0x04, tobcd(t.tm_sec), tobcd(t.tm_min), tobcd(t.tm_hour), tobcd(t.tm_mday), (uint8_t)t.tm_wday, tobcd(t.tm_mon + 1), tobcd(t.tm_year - 100) };
    i2c_master_transmit(s_dev, b, 8, 50);
}

bool rtc_time_valid(void) { return s_valid || time(NULL) > 1700000000; }
