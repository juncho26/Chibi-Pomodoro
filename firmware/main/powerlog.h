#pragma once
#include <stdbool.h>

// Where the battery goes. Counts the time spent on each screen, with Wi-Fi on, and with the
// screen lit, dimmed or off, since the device was last unplugged from a computer or charged.
// The counts live in RTC memory, so they survive deep sleep (not switching it off).
// Shown at /api/diag ("battery") and printed to the serial log once a minute.
void powerlog_tick(int dt_ms, int screen, int net_mode, int light /* 0 lit, 1 dim, 2 off */, int bright_pct);
void powerlog_note_sleep(void);           // called just before deep sleep
int  powerlog_json(char *buf, int len);
