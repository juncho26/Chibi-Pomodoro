// Chibi firmware for the Waveshare ESP32-S3-Touch-LCD-3.49.
// Boot order matters: the battery power latch is secured before anything else runs.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_task_wdt.h"
#include "power.h"
#include "store.h"
#include "imu.h"
#include "rtc.h"
#include "net.h"
#include "display.h"
#include "ui.h"
#include "app.h"
#include "weather.h"

static const char *TAG = "main";

// The UI runs on a fixed 60 Hz clock (30 Hz while the screen is dimmed): one frame of logic,
// LVGL timers, then a redraw of whatever changed. Frames are never faster than that, and the CPU
// drops to 80 MHz while it waits for the next one.
static void ui_task(void *arg)
{
    lv_display_t *disp = display_init();
    ui_init();
    app_init();
    lv_timer_pause(lv_display_get_refr_timer(disp));      // we decide when to refresh
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t cpu_lock;
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &cpu_lock);
#endif
    int64_t next = esp_timer_get_time();
    // If the UI ever hangs, the watchdog restarts the device after 8 s (and the restart log says so)
    // instead of leaving a frozen screen.
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
#if CONFIG_PM_ENABLE
        esp_pm_lock_acquire(cpu_lock);
#endif
        ui_frame();
        // LVGL wakes its own refresh timer whenever something is invalidated, which made it draw
        // (and send) a half-updated frame inside lv_timer_handler and then draw again below. Only the
        // one lv_refr_now per loop should draw.
        lv_timer_pause(lv_display_get_refr_timer(disp));
        lv_timer_handler();
        ui_render_prep();
        lv_refr_now(disp);
#if CONFIG_PM_ENABLE
        esp_pm_lock_release(cpu_lock);
#endif
        // 60 frames a second while anything moves; when nothing has been drawn for 0.4 s and no
        // finger is down (a still Tasks or Settings page), check only 20 times a second. The first
        // touch or change brings it straight back to 60.
        int64_t now = esp_timer_get_time();
        static uint32_t seen; static int64_t last_draw;
        uint32_t fc = display_flush_count();
        if (fc != seen || display_touch_active()) { seen = fc; last_draw = now; }
        const int64_t period = app_dimmed() ? 33333 : now - last_draw > 400000 ? 50000 : 16667;
        next += period;
        if (next < now - period) next = now;                 // fell behind: don't try to catch up
        int64_t wait = next - now;
        // always sleep at least one tick: a slow frame must never starve the other tasks on this core
        TickType_t ticks = wait > 0 ? pdMS_TO_TICKS((wait + 500) / 1000) : 0;
        vTaskDelay(ticks ? ticks : 1);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "start");
    power_early_init();
#if CONFIG_PM_ENABLE
    // Battery: run at 240 MHz only while drawing a frame (or while Wi-Fi/SPI need it), 80 MHz otherwise.
    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
    esp_pm_configure(&pm);
#endif
    ESP_LOGI(TAG, "latch ok");          // 1. hold SYS_EN (no expander reset, latch before direction)
    store_init(); ESP_LOGI(TAG, "store ok");               // 2. NVS: tasks, settings, session
    power_start_services(); ESP_LOGI(TAG, "services ok");      // 3. boot log, SYS_EN guard, battery, buttons
    ESP_LOGI(TAG, "wake cause: %d", (int)esp_sleep_get_wakeup_cause());
    if (!imu_init()) ESP_LOGE(TAG, "IMU missing: orientation and knocks disabled");
    imu_start();
    rtc_init_and_load(); ESP_LOGI(TAG, "rtc ok");
    net_init(); ESP_LOGI(TAG, "net ok");
    weather_start();
    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 3, NULL, 1);
}
