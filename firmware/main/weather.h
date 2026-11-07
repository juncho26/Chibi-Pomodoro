#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

// Rain forecast for today from Open-Meteo (free, no key), fetched about once an hour over home Wi-Fi.
void weather_start(void);                 // background task
void weather_poke(void);                  // settings changed: check again soon

typedef struct {
    bool rain;            // rain expected from now until midnight
    bool now;             // raining in the current hour
    int  from_hour;       // first rainy hour (when !now)
    int  max_prob;        // highest chance in the rest of the day, %
    bool snow;            // falls as snow
    float intensity;      // 0..1, how hard it rains on screen
} rain_info_t;
bool weather_rain_today(rain_info_t *out);  // false when there's no forecast for today
void weather_status(char *buf, int len);    // short status for the phone page

