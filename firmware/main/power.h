#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// Must be the FIRST thing app_main calls. Creates the sensor I2C bus and takes over the
// TCA9554 without resetting it, then makes sure SYS_EN is output-HIGH (latch first, then direction).
void power_early_init(void);

i2c_master_bus_handle_t power_i2c_bus(void);

// Read-modify-write one or more EXIO output bits using the chip's real registers (no cache).
void exio_write(uint8_t mask, bool high);
uint8_t exio_read_inputs(void);   // also clears the TCA9554 INT line

// Starts the SYS_EN guard, battery sampling and button polling.
void power_start_services(void);

// Battery
int  power_battery_pct(void);     // 0..100, -1 until the first reading
int  power_battery_mv(void);
bool power_on_usb(void);          // USB data host connected (computer); charging likely
bool power_battery_critical(void);

// Buttons -> events
typedef enum { BTN_NONE, BTN_BOOT_SHORT, BTN_BOOT_LONG, BTN_PWR_SHORT, BTN_PWR_LONG } btn_event_t;
btn_event_t power_button_event(void);   // non-blocking, call from the UI loop

// Releases SYS_EN. On battery the board turns off. Returns only if it is still running (USB power).
void power_off(void);

// Diagnostics (boot log). Writes JSON into buf.
void power_diag_json(char *buf, int len);
void power_note_alive(void);       // cheap; persists a breadcrumb every few minutes
