#pragma once
#include <stdint.h>
#define ESP_EXT1_WAKEUP_ANY_LOW 0
static inline void esp_sleep_enable_ext1_wakeup_io(uint64_t m, int l) { (void)m; (void)l; }
void esp_deep_sleep_start(void);
