// AXS15231B QSPI panel (native 172x640 portrait) + touch + LVGL v9.
// The panel only takes whole frames written top to bottom, so LVGL renders full frames and the flush
// sends them in 64-row chunks (same method as Waveshare's examples).
#include "display.h"
#include "board.h"
#include "power.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"

static const char *TAG = "display";
#define CHUNK_ROWS 64
#define CHUNK_BYTES (LCD_NATIVE_W * CHUNK_ROWS * 2)
#define FRAME_BYTES (LCD_NATIVE_W * LCD_NATIVE_H * 2)

// ---------------------------------------------------------------------------------------------
// Frame pipeline (aims for a steady 60 fps, capped there, and no work when nothing changes):
//   core 1  LVGL redraws only the invalidated areas into one logical-orientation frame buffer
//           (DIRECT mode), then converts only what changed since that staging buffer was last
//           used (rotate + byte swap, cache-friendly tiles) into one of two native-layout
//           staging buffers, and immediately starts on the next frame.
//   core 0  a sender task streams the staging buffer to the panel in 64-row chunks through two
//           internal DMA buffers that alternate, so copying and the QSPI transfer overlap.
// The panel only accepts whole frames written top to bottom, so every frame sends all rows.
// ---------------------------------------------------------------------------------------------
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static uint8_t *s_fb;                    // LVGL frame buffer (logical orientation), PSRAM
static uint16_t *s_stage[2];             // native layout, byte-swapped, PSRAM
static uint16_t *s_dma[2];               // internal DMA chunk buffers
static SemaphoreHandle_t s_stage_free[2], s_dma_free;
static QueueHandle_t s_send_q;
typedef struct { int idx, rows; } send_job_t;   // which stage buffer, and how many rows from the top to send
static int s_rows_owed;                         // rows changed but not yet sent (a dropped frame)
static bool s_stage_valid[2];
static int s_stage_next;
static lv_area_t s_dirty, s_dirty_prev;  // union of areas redrawn this frame / last frame
// The separate areas too, so a frame that changes two small far-apart things (rain drops, the eyes
// and the battery) copies just those, not the big box around them. Too many: use the union.
#define MAX_AREAS 24
static lv_area_t s_ar[MAX_AREAS], s_ar_prev[MAX_AREAS];
static int s_nar, s_nar_prev = -1;                // -1: too many areas, use the union
static bool s_dirty_any, s_dirty_prev_any;
static volatile bool s_paused;
static volatile int s_sender_at = -1;   // for the log: -1 idle, 0..9 queueing that chunk, 100 waiting for the last one
static i2c_master_bus_handle_t s_tp_bus;
static i2c_master_dev_handle_t s_tp;
static volatile bool s_touching;

// ---- frame statistics (for /api/diag and the serial log) ----
static struct {
    int64_t refr_start, last_frame, win_start;
    uint32_t frames, win_frames;
    uint32_t render_us, conv_us, send_us;      // sums over the window
    uint32_t worst_gap_us;
    char tag[16];
    // last finished window
    float fps, render_ms, conv_ms, send_ms, worst_ms;
    char last_tag[16];
    uint32_t stalls, send_errs;               // LCD transfers that had to be recovered
} ST;

static const axs15231b_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_free, &w);
    return w == pdTRUE;
}

static void area_union(lv_area_t *r, const lv_area_t *a, const lv_area_t *b)
{
    lv_area_t o = { a->x1 < b->x1 ? a->x1 : b->x1, a->y1 < b->y1 ? a->y1 : b->y1, a->x2 > b->x2 ? a->x2 : b->x2, a->y2 > b->y2 ? a->y2 : b->y2 };
    *r = o;
}

static inline uint16_t sw16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// Copy logical rect [x0,x1]x[y0,y1] of the frame buffer into a native-layout staging buffer,
// rotating and byte-swapping on the way. 32x32 tiles keep both sides in the PSRAM cache.
static void convert_rect(uint16_t *dst, lv_display_rotation_t rot, int lw, int lh, int x0, int y0, int x1, int y1)
{
    const uint16_t *src = (const uint16_t *)s_fb;
    const int NW = LCD_NATIVE_W;
    if (rot == LV_DISPLAY_ROTATION_0 || rot == LV_DISPLAY_ROTATION_180) {
        // portrait: rows stay rows, so copy them straight through (no tiles needed), two pixels at a time
        for (int y = y0; y <= y1; y++) {
            const uint16_t *s = src + y * lw;
            if (rot == LV_DISPLAY_ROTATION_0) {
                uint16_t *d = dst + y * NW; int x = x0;
                if ((x & 1) && x <= x1) { d[x] = sw16(s[x]); x++; }
                for (; x + 1 <= x1; x += 2) {
                    uint32_t v = *(const uint32_t *)(s + x);
                    *(uint32_t *)(d + x) = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
                }
                if (x <= x1) d[x] = sw16(s[x]);
            } else {
                uint16_t *d = dst + (lh - 1 - y) * NW + (lw - 1);
                for (int x = x0; x <= x1; x++) *(d - x) = sw16(s[x]);
            }
        }
        return;
    }
    for (int ty = y0; ty <= y1; ty += 32) {
        int ye = ty + 31 > y1 ? y1 : ty + 31;
        for (int tx = x0; tx <= x1; tx += 32) {
            int xe = tx + 31 > x1 ? x1 : tx + 31;
            for (int y = ty; y <= ye; y++) {
                const uint16_t *s = src + y * lw;
                switch (rot) {
                case LV_DISPLAY_ROTATION_90:  for (int x = tx; x <= xe; x++) dst[(lw - 1 - x) * NW + y] = sw16(s[x]); break;
                case LV_DISPLAY_ROTATION_270: for (int x = tx; x <= xe; x++) dst[x * NW + (lh - 1 - y)] = sw16(s[x]); break;
                case LV_DISPLAY_ROTATION_180: { uint16_t *d = dst + (lh - 1 - y) * NW + (lw - 1); for (int x = tx; x <= xe; x++) *(d - x) = sw16(s[x]); break; }
                default:                      { uint16_t *d = dst + y * NW; for (int x = tx; x <= xe; x++) d[x] = sw16(s[x]); break; }
                }
            }
        }
    }
}

static void sender_task(void *arg)
{
    send_job_t job; int idx;
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t lock;
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "lcd", &lock);
#endif
    for (;;) {
        xQueueReceive(s_send_q, &job, portMAX_DELAY);
        idx = job.idx;
#if CONFIG_PM_ENABLE
        esp_pm_lock_acquire(lock);
#endif
        int64_t t0 = esp_timer_get_time();
        const uint8_t *p = (const uint8_t *)s_stage[idx];
        // Never wait forever: a transfer that fails or whose "done" signal is lost must not freeze
        // the screen. After 250 ms the chunk buffers are taken back and the next frame carries on.
        bool stalled = false;
        // One chunk on the wire at a time (as in Waveshare's own example: several queued transfers
        // mixed with the panel's address commands locked the bus on real boards), but the next chunk
        // is copied into the other buffer while this one is being sent, so the copy costs no time.
        // Only the rows down to the lowest change are sent: the panel is written from the top, and
        // everything below that line is already on it. With the character moving at one end of the
        // screen, that is often a third of the frame or less.
        const int NCH = (job.rows + CHUNK_ROWS - 1) / CHUNK_ROWS;
        memcpy(s_dma[0], p, CHUNK_BYTES);
        for (int c = 0; c < NCH; c++) {
            if (xSemaphoreTake(s_dma_free, pdMS_TO_TICKS(250)) != pdTRUE) { stalled = true; break; }   // previous chunk sent
            s_sender_at = c;
            int y = c * CHUNK_ROWS;
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_NATIVE_W, y + CHUNK_ROWS, s_dma[c & 1]) != ESP_OK) {
                ST.send_errs++; xSemaphoreGive(s_dma_free);     // never queued: no "done" will come for it
            }
            if (c + 1 < NCH) memcpy(s_dma[(c + 1) & 1], p + (size_t)(c + 1) * CHUNK_BYTES, CHUNK_BYTES);   // while chunk c is on the wire
        }
        // the last chunk back = the whole frame is on the panel
        s_sender_at = 100;
        if (!stalled) stalled = xSemaphoreTake(s_dma_free, pdMS_TO_TICKS(250)) != pdTRUE;
        if (!stalled) xSemaphoreGive(s_dma_free);
        s_sender_at = -1;
        if (stalled) { ST.stalls++; ESP_LOGW(TAG, "LCD transfer stalled (%lu), recovering", (unsigned long)ST.stalls); }
        while (uxSemaphoreGetCount(s_dma_free) < 1) xSemaphoreGive(s_dma_free);
        ST.send_us += (uint32_t)(esp_timer_get_time() - t0);
#if CONFIG_PM_ENABLE
        esp_pm_lock_release(lock);
#endif
        xSemaphoreGive(s_stage_free[idx]);
    }
}

static void stats_frame_done(int64_t now)
{
    if (ST.last_frame) {
        uint32_t gap = (uint32_t)(now - ST.last_frame);
        if (gap < 500000 && gap > ST.worst_gap_us) ST.worst_gap_us = gap;   // ignore idle pauses
    }
    ST.last_frame = now;
    ST.frames++; ST.win_frames++;
    if (!ST.win_start) ST.win_start = now;
    int64_t span = now - ST.win_start;
    if (span >= 5000000) {
        float n = ST.win_frames ? ST.win_frames : 1;
        ST.fps = ST.win_frames * 1e6f / span;
        ST.render_ms = ST.render_us / n / 1000.f; ST.conv_ms = ST.conv_us / n / 1000.f; ST.send_ms = ST.send_us / n / 1000.f;
        ST.worst_ms = ST.worst_gap_us / 1000.f;
        snprintf(ST.last_tag, sizeof ST.last_tag, "%s", ST.tag);
        ESP_LOGI(TAG, "%s: %.1f fps (slowest frame %.1f ms) render %.1f ms, convert %.1f ms, send %.1f ms",
                 ST.last_tag, ST.fps, ST.worst_ms, ST.render_ms, ST.conv_ms, ST.send_ms);
        ST.win_start = now; ST.win_frames = 0; ST.render_us = ST.conv_us = ST.send_us = 0; ST.worst_gap_us = 0;
    }
}

static void refr_start_cb(lv_event_t *e) { ST.refr_start = esp_timer_get_time(); }

static volatile uint32_t s_flushes;
uint32_t display_flush_count(void) { return s_flushes; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (lv_display_flush_is_last(disp)) s_flushes++;
    if (!s_dirty_any) { s_dirty = *area; s_dirty_any = true; s_nar = 0; }
    else area_union(&s_dirty, &s_dirty, area);
    if (s_nar >= 0 && s_nar < MAX_AREAS) s_ar[s_nar++] = *area; else s_nar = -1;
    if (!lv_display_flush_is_last(disp)) { lv_display_flush_ready(disp); return; }

    int64_t t0 = esp_timer_get_time();
    ST.render_us += (uint32_t)(t0 - ST.refr_start);
    if (s_paused) {                       // screen off: nothing is sent; redraw everything on resume
        s_stage_valid[0] = s_stage_valid[1] = false;
        s_dirty_any = false;
        lv_display_flush_ready(disp);
        return;
    }
    int idx = s_stage_next; s_stage_next ^= 1;
    if (xSemaphoreTake(s_stage_free[idx], pdMS_TO_TICKS(600)) != pdTRUE)   // that buffer has finished sending
        ESP_LOGW(TAG, "stage buffer still busy (sender at %d), drawing anyway", s_sender_at);
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    int lw = lv_display_get_horizontal_resolution(disp), lh = lv_display_get_vertical_resolution(disp);
    // what changed since this stage buffer was last filled (two frames ago): this frame's areas and
    // the last frame's. Each is copied on its own; the panel rows sent run down to the lowest of them.
    lv_area_t list[2 * MAX_AREAS + 1]; int nl = 0;
    if (!s_stage_valid[idx]) list[nl++] = (lv_area_t){ 0, 0, lw - 1, lh - 1 };
    else if (s_nar < 0 || s_nar_prev < 0) { list[nl] = s_dirty; if (s_dirty_prev_any) area_union(&list[nl], &list[nl], &s_dirty_prev); nl++; }
    else {
        for (int i = 0; i < s_nar; i++) list[nl++] = s_ar[i];
        if (s_dirty_prev_any) for (int i = 0; i < s_nar_prev; i++) list[nl++] = s_ar_prev[i];
    }
    int last = 0;
    for (int i = 0; i < nl; i++) {
        lv_area_t a = list[i];
        if (a.x1 < 0) a.x1 = 0;
        if (a.y1 < 0) a.y1 = 0;
        if (a.x2 > lw - 1) a.x2 = lw - 1;
        if (a.y2 > lh - 1) a.y2 = lh - 1;
        if (a.x1 > a.x2 || a.y1 > a.y2) continue;
        convert_rect(s_stage[idx], rot, lw, lh, a.x1, a.y1, a.x2, a.y2);
        // the lowest panel row this area touches (panel rows run along the long side)
        int l;
        switch (rot) {
        case LV_DISPLAY_ROTATION_270: l = a.x2; break;
        case LV_DISPLAY_ROTATION_90:  l = lw - 1 - a.x1; break;
        case LV_DISPLAY_ROTATION_180: l = lh - 1 - a.y1; break;
        default:                      l = a.y2; break;
        }
        if (l > last) last = l;
    }
    int rows = last + 1;
    if (rows < s_rows_owed) rows = s_rows_owed;
    if (rows > LCD_NATIVE_H || rows < 1) rows = LCD_NATIVE_H;
    s_stage_valid[idx] = true;
    s_dirty_prev = s_dirty; s_dirty_prev_any = true; s_dirty_any = false;
    s_nar_prev = s_nar; if (s_nar > 0) memcpy(s_ar_prev, s_ar, sizeof(lv_area_t) * s_nar);
    int64_t t1 = esp_timer_get_time();
    ST.conv_us += (uint32_t)(t1 - t0);
    lv_display_flush_ready(disp);                            // LVGL may start the next frame now
    send_job_t job = { idx, rows };
    if (xQueueSend(s_send_q, &job, pdMS_TO_TICKS(600)) != pdTRUE) { xSemaphoreGive(s_stage_free[idx]); s_rows_owed = LCD_NATIVE_H; }   // sender stuck: drop this frame, send everything next time
    else s_rows_owed = 0;
    stats_frame_done(t1);
}

void display_stats_tag(const char *name) { snprintf(ST.tag, sizeof ST.tag, "%s", name); }

int display_stats_json(char *buf, int len)
{
    return snprintf(buf, len, "{\"screen\":\"%s\",\"fps\":%.1f,\"slowestFrameMs\":%.1f,\"renderMs\":%.1f,\"convertMs\":%.1f,\"sendMs\":%.1f,\"frames\":%lu,\"lcdStalls\":%lu,\"lcdErrors\":%lu}",
                    ST.last_tag, ST.fps, ST.worst_ms, ST.render_ms, ST.conv_ms, ST.send_ms, (unsigned long)ST.frames, (unsigned long)ST.stalls, (unsigned long)ST.send_errs);
}

// Screen off: stop drawing and sending entirely. On resume the whole screen is redrawn once.
void display_pause(bool pause)
{
    if (pause == s_paused) return;
    s_paused = pause;
    if (!s_disp) return;
    lv_display_enable_invalidation(s_disp, !pause);
    if (!pause) { s_stage_valid[0] = s_stage_valid[1] = false; lv_obj_invalidate(lv_screen_active()); ST.last_frame = 0; }
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t b[14] = {0};
    if (i2c_master_transmit_receive(s_tp, cmd, sizeof cmd, b, sizeof b, 20) != ESP_OK) { data->state = LV_INDEV_STATE_RELEASED; s_touching = false; return; }
    uint16_t px = ((b[2] & 0x0F) << 8) | b[3];
    uint16_t py = ((b[4] & 0x0F) << 8) | b[5];
    if (b[1] > 0 && b[1] < 5) {
        if (px > LCD_NATIVE_H) px = LCD_NATIVE_H;
        if (py > LCD_NATIVE_W) py = LCD_NATIVE_W;
        data->point.x = py;                       // native panel coordinates; LVGL applies the rotation
        data->point.y = LCD_NATIVE_H - px;
        data->state = LV_INDEV_STATE_PRESSED;
        s_touching = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_touching = false;
    }
}

bool display_touch_active(void) { return s_touching; }

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void backlight_hw_init(void)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT, .timer_num = LEDC_TIMER_3, .freq_hz = 50000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    // duty 255 = pin HIGH = darkest
    ledc_channel_config_t c = { .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_3, .duty = 255, .hpoint = 0 };
    ledc_channel_config(&c);
}

void display_backlight(int level)
{
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    // The panel's backlight shows nothing below about 40% drive, so the whole brightness range
    // (10%..100% in Settings, and the 10% dim level) is spread over the part that actually lights.
    #define BL_HW_MIN 96
    if (level > 0) level = BL_HW_MIN + level * (255 - BL_HW_MIN) / 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 255 - level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
#if CONFIG_CHIBI_BOARD_V2
    exio_write(EXIO_BL_EN, level > 0);
#endif
}

void display_panel_sleep(bool sleep)
{
    if (!s_panel) return;
    if (sleep) { esp_lcd_panel_disp_on_off(s_panel, false); esp_lcd_panel_disp_sleep(s_panel, true); }
    else { esp_lcd_panel_disp_sleep(s_panel, false); vTaskDelay(pdMS_TO_TICKS(120)); esp_lcd_panel_disp_on_off(s_panel, true); }
}

void display_set_orientation(orient_t o)
{
    static const lv_display_rotation_t map[4] = { LV_DISPLAY_ROTATION_270, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_180 };
    if (o < 0 || o > 3) return;
    if (lv_display_get_rotation(s_disp) == map[o]) return;
    lv_display_set_rotation(s_disp, map[o]);
    // LVGL 9.2 keeps the old row stride after a rotation; re-register the buffer so it's recomputed.
    lv_display_set_buffers(s_disp, s_fb, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    s_stage_valid[0] = s_stage_valid[1] = false;
    s_dirty_prev_any = false; s_nar_prev = -1;
    lv_obj_invalidate(lv_screen_active());
}

lv_display_t *display_init(void)
{
    backlight_hw_init();
    display_backlight(0);

    // LCD reset
#if CONFIG_CHIBI_BOARD_V2
    exio_write(EXIO_LCD_RST, true);  vTaskDelay(pdMS_TO_TICKS(30));
    exio_write(EXIO_LCD_RST, false); vTaskDelay(pdMS_TO_TICKS(250));
    exio_write(EXIO_LCD_RST, true);  vTaskDelay(pdMS_TO_TICKS(30));
#else
    gpio_config_t g = { .pin_bit_mask = 1ULL << PIN_LCD_RST, .mode = GPIO_MODE_OUTPUT, .pull_up_en = 1 };
    gpio_config(&g);
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
#endif

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_CLK, .data0_io_num = PIN_LCD_D0, .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2, .data3_io_num = PIN_LCD_D3, .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = -1, .spi_mode = 3, .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10, .on_color_trans_done = on_trans_done, .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &ioc, &io));
    axs15231b_vendor_config_t vc = { .flags.use_qspi_interface = 1, .init_cmds = s_init_cmds, .init_cmds_size = 2 };
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vc };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &pc, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // touch bus
    i2c_master_bus_config_t tb = { .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_1, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&tb, &s_tp_bus));
    i2c_device_config_t td = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR_TOUCH, .scl_speed_hz = 300000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_tp_bus, &td, &s_tp));

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(LCD_NATIVE_W, LCD_NATIVE_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    s_fb = heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) {
        s_stage[i] = heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
        s_dma[i] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_stage_free[i] = xSemaphoreCreateBinary(); xSemaphoreGive(s_stage_free[i]);
    }
    assert(s_fb && s_stage[0] && s_stage[1] && s_dma[0] && s_dma[1]);
    memset(s_fb, 0, FRAME_BYTES);
    s_dma_free = xSemaphoreCreateCounting(1, 1);
    s_send_q = xQueueCreate(2, sizeof(send_job_t));
    xTaskCreatePinnedToCore(sender_task, "lcd_send", 4096, NULL, 5, NULL, 0);
    lv_display_set_buffers(s_disp, s_fb, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, refr_start_cb, LV_EVENT_REFR_START, NULL);

    lv_indev_t *ind = lv_indev_create();
    lv_indev_set_type(ind, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ind, touch_read);
    lv_indev_set_long_press_time(ind, 1000);   // "hold" gestures need a full second
    lv_timer_set_period(lv_indev_get_read_timer(ind), 20);   // 50 touch reads a second is plenty (each is an I2C transfer)
    ESP_LOGI(TAG, "display ready");
    return s_disp;
}
