// Waveshare ESP32-S3-Touch-LCD-3.49 pin map.
// Source: waveshareteam/ESP32-S3-Touch-LCD-3.49-V2 (user_config.h + V2 schematic),
//         waveshareteam/ESP32-S3-Touch-LCD-3.49 (V1 examples).
#pragma once
#include "sdkconfig.h"
#include "driver/gpio.h"

// ---- shared sensor bus: TCA9554 + QMI8658 + PCF85063 ----
#define PIN_I2C_SDA        GPIO_NUM_47
#define PIN_I2C_SCL        GPIO_NUM_48
#define ADDR_TCA9554       0x20
#define ADDR_QMI8658       0x6B
#define ADDR_PCF85063      0x51

// ---- touch (AXS15231B touch part) on its own bus ----
#define PIN_TP_SDA         GPIO_NUM_17
#define PIN_TP_SCL         GPIO_NUM_18
#define ADDR_TOUCH         0x3B

// ---- LCD, QSPI ----
#define PIN_LCD_CS         GPIO_NUM_9
#define PIN_LCD_CLK        GPIO_NUM_10
#define PIN_LCD_D0         GPIO_NUM_11
#define PIN_LCD_D1         GPIO_NUM_12
#define PIN_LCD_D2         GPIO_NUM_13
#define PIN_LCD_D3         GPIO_NUM_14
#define LCD_NATIVE_W       172
#define LCD_NATIVE_H       640

#if CONFIG_CHIBI_BOARD_V2
#define PIN_LCD_BL         GPIO_NUM_42   // PWM into the boost converter FB: HIGH = dimmer
#define PIN_LCD_RST        (-1)          // on EXIO5
#define PIN_EXIO_INT       GPIO_NUM_8    // TCA9554 INT (open drain, active low)
#else
#define PIN_LCD_BL         GPIO_NUM_8
#define PIN_LCD_RST        GPIO_NUM_21
#define PIN_EXIO_INT       GPIO_NUM_42
#endif

// ---- buttons ----
#define PIN_BOOT_BTN       GPIO_NUM_0    // active low
#define PIN_PWR_BTN        GPIO_NUM_16   // "SYS_OUT": reads LOW only while PWR is held

// ---- battery sense: ADC1 channel 3 (GPIO4), divider x3 ----
#define BAT_ADC_CHANNEL    ADC_CHANNEL_3
#define BAT_DIVIDER        3.0f

// ---- TCA9554 EXIO bits (V2 map) ----
#define EXIO_TP_INT        (1u << 0)
#define EXIO_BL_EN         (1u << 1)
#define EXIO_IMU_INT1      (1u << 2)
#define EXIO_IMU_INT2      (1u << 3)
#define EXIO_RTC_INT       (1u << 4)
#define EXIO_LCD_RST       (1u << 5)
#define EXIO_SYS_EN        (1u << 6)     // battery power hold. NEVER let this become an input.
#define EXIO_NS_MODE       (1u << 7)
