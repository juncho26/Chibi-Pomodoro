// Power latch, battery and buttons.
//
// Why this file exists: on this board the battery stays connected only while TCA9554 EXIO6 (SYS_EN)
// is an OUTPUT driven HIGH. The stock esp_io_expander_tca9554 driver writes DIR=0xFF ("all inputs")
// when it is created, which releases SYS_EN and cuts power on every reset, crash or deep-sleep wake
// when running on battery. This driver never resets the chip. It reads the real registers, sets the
// SYS_EN output latch HIGH first, and only then switches the pin to output.
#include "power.h"
#include "board.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "nvs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "power";

#define TCA_IN   0x00
#define TCA_OUT  0x01
#define TCA_POL  0x02
#define TCA_CFG  0x03   // 1 = input

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tca;
static SemaphoreHandle_t s_tca_lock;
static volatile bool s_powering_off;
static uint32_t s_guard_fixes;

i2c_master_bus_handle_t power_i2c_bus(void) { return s_bus; }

static esp_err_t tca_rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_tca, &reg, 1, v, 1, 50);
}
static esp_err_t tca_wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_tca, b, 2, 50);
}

// The safe order: output latch first, then direction.
static bool tca_assert_outputs(uint8_t high_mask, uint8_t low_mask, uint8_t output_mask)
{
    uint8_t out, cfg;
    if (tca_rd(TCA_OUT, &out) != ESP_OK) return false;
    out = (out | high_mask) & ~low_mask;
    if (tca_wr(TCA_OUT, out) != ESP_OK) return false;
    if (tca_rd(TCA_CFG, &cfg) != ESP_OK) return false;
    cfg &= ~output_mask;
    return tca_wr(TCA_CFG, cfg) == ESP_OK;
}

void exio_write(uint8_t mask, bool high)
{
    if (!s_tca) return;
    if ((mask & EXIO_SYS_EN) && !high) {
        ESP_LOGE(TAG, "refusing to drive SYS_EN low outside power_off()");
        mask &= ~EXIO_SYS_EN;
    }
    xSemaphoreTake(s_tca_lock, portMAX_DELAY);
    tca_assert_outputs(high ? mask : 0, high ? 0 : mask, mask);
    xSemaphoreGive(s_tca_lock);
}

uint8_t exio_read_inputs(void)
{
    uint8_t v = 0xFF;
    if (!s_tca) return v;
    xSemaphoreTake(s_tca_lock, portMAX_DELAY);
    tca_rd(TCA_IN, &v);
    xSemaphoreGive(s_tca_lock);
    return v;
}

void power_early_init(void)
{
    s_tca_lock = xSemaphoreCreateMutex();
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR_TCA9554, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dc, &s_tca));

    uint8_t out = 0, cfg = 0;
    bool ok = false;
    for (int i = 0; i < 5 && !ok; i++) {
        uint8_t high = EXIO_SYS_EN, low = 0, outputs = EXIO_SYS_EN;
#if CONFIG_CHIBI_BOARD_V2
        high |= EXIO_LCD_RST;              // keep the LCD out of reset
        low |= EXIO_BL_EN;                 // backlight off until the first frame is drawn
        outputs |= EXIO_LCD_RST | EXIO_BL_EN;
#endif
        ok = tca_assert_outputs(high, low, outputs);
        if (!ok) vTaskDelay(pdMS_TO_TICKS(2));
    }
    tca_rd(TCA_OUT, &out);
    tca_rd(TCA_CFG, &cfg);
    ESP_LOGI(TAG, "TCA9554 taken over without reset: OUT=0x%02x CFG=0x%02x, SYS_EN %s", out, cfg,
             ((out & EXIO_SYS_EN) && !(cfg & EXIO_SYS_EN)) ? "held" : "NOT HELD");
}

// ---------------------------------------------------------------- diagnostics
// RTC_NOINIT survives software resets, panics and watchdog resets (not power loss).
RTC_NOINIT_ATTR static uint32_t rtc_magic, rtc_uptime_s, rtc_mv, rtc_sys_en_ok;
#define RTC_MAGIC 0xC0DE5EED

typedef struct { uint32_t boots; uint8_t reason[8]; uint16_t mv[8]; uint32_t up[8]; uint8_t sys_en[8]; uint8_t head; } boot_log_t;
static boot_log_t s_log;

static void boot_log_record(void)
{
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) != ESP_OK) return;
    size_t sz = sizeof(s_log);
    if (nvs_get_blob(h, "log", &s_log, &sz) != ESP_OK || sz != sizeof(s_log)) memset(&s_log, 0, sizeof(s_log));
    uint32_t prev_up = 0, prev_mv = 0, prev_ok = 1;
    if (rtc_magic == RTC_MAGIC) { prev_up = rtc_uptime_s; prev_mv = rtc_mv; prev_ok = rtc_sys_en_ok; }
    else {
        uint32_t v;
        if (nvs_get_u32(h, "alive_up", &v) == ESP_OK) prev_up = v;
        if (nvs_get_u32(h, "alive_mv", &v) == ESP_OK) prev_mv = v;
    }
    uint8_t i = s_log.head % 8;
    s_log.reason[i] = (uint8_t)esp_reset_reason();
    s_log.mv[i] = (uint16_t)prev_mv;
    s_log.up[i] = prev_up;
    s_log.sys_en[i] = (uint8_t)prev_ok;
    s_log.head = (i + 1) % 8;
    s_log.boots++;
    nvs_set_blob(h, "log", &s_log, sizeof(s_log));
    nvs_commit(h);
    nvs_close(h);
    rtc_magic = RTC_MAGIC; rtc_uptime_s = 0; rtc_mv = 0; rtc_sys_en_ok = 1;
    ESP_LOGW(TAG, "boot #%lu, reset reason %d, previous run: %lus, %lumV",
             (unsigned long)s_log.boots, (int)esp_reset_reason(), (unsigned long)prev_up, (unsigned long)prev_mv);
}

static const char *reason_name(int r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "reset button";
    default: return "other";
    }
}

void power_diag_json(char *buf, int len)
{
    int n = snprintf(buf, len, "{\"boots\":%lu,\"guardFixes\":%lu,\"batteryMv\":%d,\"usb\":%s,\"log\":[",
                     (unsigned long)s_log.boots, (unsigned long)s_guard_fixes, power_battery_mv(), power_on_usb() ? "true" : "false");
    for (int k = 0; k < 8 && n < len - 120; k++) {
        int i = (s_log.head + 7 - k) % 8;
        if (!s_log.up[i] && !s_log.reason[i]) continue;
        n += snprintf(buf + n, len - n, "%s{\"reason\":\"%s\",\"prevUptimeS\":%lu,\"prevMv\":%u,\"sysEnHeld\":%s}",
                      k ? "," : "", reason_name(s_log.reason[i]), (unsigned long)s_log.up[i], s_log.mv[i], s_log.sys_en[i] ? "true" : "false");
    }
    snprintf(buf + n, len - n, "]}");
}

void power_note_alive(void)
{
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    rtc_uptime_s = (uint32_t)(now / 1000000);
    rtc_mv = power_battery_mv();
    if (now - last < 300LL * 1000000) return;   // NVS breadcrumb every 5 minutes
    last = now;
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "alive_up", rtc_uptime_s);
        nvs_set_u32(h, "alive_mv", rtc_mv);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------- battery
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_mv = -1, s_pct = -1, s_low_count;

static int pct_from_mv(int mv);
int power_pct_from_mv(int mv) { return pct_from_mv(mv); }
static int pct_from_mv(int mv)
{
    static const int tbl[][2] = { {4180,100},{4100,92},{4000,82},{3900,68},{3800,52},{3750,42},{3700,32},{3650,22},{3600,13},{3500,6},{3400,2},{3300,0} };
    if (mv >= tbl[0][0]) return 100;
    for (int i = 1; i < 12; i++)
        if (mv >= tbl[i][0]) return tbl[i][1] + (tbl[i-1][1] - tbl[i][1]) * (mv - tbl[i][0]) / (tbl[i-1][0] - tbl[i][0]);
    return 0;
}

static void battery_sample(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) return;
    if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv); else mv = raw * 3300 / 4095;
    mv = (int)(mv * BAT_DIVIDER);
    s_mv = s_mv < 0 ? mv : (s_mv * 7 + mv) / 8;
    int p = pct_from_mv(s_mv);
    // hysteresis so the number doesn't flicker
    if (s_pct < 0 || p > s_pct + 1 || p < s_pct - 1) s_pct = p;
    s_low_count = (s_mv < 3380 && !power_on_usb()) ? s_low_count + 1 : 0;
}

int  power_battery_pct(void) { return s_pct; }
int  power_battery_mv(void) { return s_mv; }
bool power_on_usb(void) { return usb_serial_jtag_is_connected(); }
bool power_battery_critical(void) { return s_low_count >= 20; }   // 20 s below 3.38 V

// ---------------------------------------------------------------- buttons
static QueueHandle_t s_btn_q;
typedef struct { int pin; int64_t down_at; bool down; bool armed; bool long_sent; int long_ms; btn_event_t ev_short, ev_long; } btn_t;
static btn_t s_btn[2] = {
    { .pin = PIN_BOOT_BTN, .armed = true,  .long_ms = 1200, .ev_short = BTN_BOOT_SHORT, .ev_long = BTN_BOOT_LONG },
    { .pin = PIN_PWR_BTN,  .armed = false, .long_ms = 3000, .ev_short = BTN_PWR_SHORT,  .ev_long = BTN_PWR_LONG },
};

static void buttons_poll(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < 2; i++) {
        btn_t *b = &s_btn[i];
        bool pressed = gpio_get_level(b->pin) == 0;
        if (!b->armed) {               // PWR: ignore the press that switched the board on
            if (!pressed) b->armed = true;
            continue;
        }
        if (pressed && !b->down) { b->down = true; b->down_at = now; b->long_sent = false; }
        else if (pressed && b->down && !b->long_sent && now - b->down_at >= b->long_ms) {
            b->long_sent = true; btn_event_t e = b->ev_long; xQueueSend(s_btn_q, &e, 0);
        } else if (!pressed && b->down) {
            b->down = false;
            if (!b->long_sent && now - b->down_at >= 40) { btn_event_t e = b->ev_short; xQueueSend(s_btn_q, &e, 0); }
        }
    }
}

btn_event_t power_button_event(void)
{
    btn_event_t e = BTN_NONE;
    if (s_btn_q) xQueueReceive(s_btn_q, &e, 0);
    return e;
}

// ---------------------------------------------------------------- guard + services
static void power_task(void *arg)
{
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        battery_sample();
        power_note_alive();
        if (++tick % 2 || s_powering_off) continue;
        // SYS_EN guard: check the PHYSICAL registers every 2 s, never a cached value.
        uint8_t out = 0, cfg = 0xFF;
        xSemaphoreTake(s_tca_lock, portMAX_DELAY);
        bool rd = tca_rd(TCA_OUT, &out) == ESP_OK && tca_rd(TCA_CFG, &cfg) == ESP_OK;
        bool held = rd && (out & EXIO_SYS_EN) && !(cfg & EXIO_SYS_EN);
        if (rd && !held) {
            s_guard_fixes++;
            tca_assert_outputs(EXIO_SYS_EN, 0, EXIO_SYS_EN);
            ESP_LOGE(TAG, "SYS_EN was released (OUT=0x%02x CFG=0x%02x), re-asserted", out, cfg);
        }
        xSemaphoreGive(s_tca_lock);
        rtc_sys_en_ok = held || !rd;
    }
}

void power_start_services(void)
{
    boot_log_record();

    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&uc, &s_adc);
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .chan = BAT_ADC_CHANNEL, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
    for (int i = 0; i < 8; i++) battery_sample();

    gpio_config_t io = { .pin_bit_mask = (1ULL << PIN_BOOT_BTN) | (1ULL << PIN_PWR_BTN), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    s_btn_q = xQueueCreate(8, sizeof(btn_event_t));
    // PWR is "pre-armed" only if it's not currently held (i.e. we booted from USB or a wake source)
    s_btn[1].armed = gpio_get_level(PIN_PWR_BTN) != 0;
    const esp_timer_create_args_t ta = { .callback = buttons_poll, .name = "btn" };
    esp_timer_handle_t t;
    esp_timer_create(&ta, &t);
    esp_timer_start_periodic(t, 10 * 1000);

    xTaskCreatePinnedToCore(power_task, "power", 4096, NULL, 5, NULL, 1);
}

void power_off(void)
{
    ESP_LOGW(TAG, "power off requested");
    s_powering_off = true;
#if CONFIG_CHIBI_BOARD_V2
    exio_write(EXIO_BL_EN, false);
#endif
    xSemaphoreTake(s_tca_lock, portMAX_DELAY);
    uint8_t out;
    if (tca_rd(TCA_OUT, &out) == ESP_OK) tca_wr(TCA_OUT, out & ~EXIO_SYS_EN);
    xSemaphoreGive(s_tca_lock);
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Still alive: USB is powering the board. Hold the latch again and carry on.
    ESP_LOGW(TAG, "still powered (USB). Keeping SYS_EN held.");
    xSemaphoreTake(s_tca_lock, portMAX_DELAY);
    tca_assert_outputs(EXIO_SYS_EN, 0, EXIO_SYS_EN);
    xSemaphoreGive(s_tca_lock);
#if CONFIG_CHIBI_BOARD_V2
    exio_write(EXIO_BL_EN, true);
#endif
    s_powering_off = false;
}
