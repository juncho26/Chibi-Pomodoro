#pragma once
#include <stdbool.h>
#include <time.h>
bool rtc_init_and_load(void);
void rtc_set_epoch(time_t epoch);   // sets system time and the RTC chip
bool rtc_time_valid(void);
void rtc_set_tz_offset_min(int minutes_east_of_utc);
