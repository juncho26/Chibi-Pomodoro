#pragma once
#include "driver/gpio.h"
static inline int rtc_gpio_pullup_en(int p) { (void)p; return 0; }
static inline int rtc_gpio_pulldown_dis(int p) { (void)p; return 0; }
