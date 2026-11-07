#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { NET_OFF, NET_STARTING, NET_STA, NET_AP } net_mode_t;

void net_init(void);                 // once at boot (netif + event loop), Wi-Fi stays off
void net_start(void);                // idempotent; runs in the background
void net_stop(void);
// Background internet use (weather): joins home Wi-Fi only, never the hotspot, never wakes the screen.
bool net_sta_acquire(int timeout_ms); // blocks; true when online through home Wi-Fi
void net_sta_release(void);           // done; the app turns Wi-Fi off again unless Tasks is open
bool net_quiet_finished(void);
net_mode_t net_mode(void);
int64_t net_last_activity_ms(void);  // last HTTP request
// What the Tasks screen shows. Valid while mode is NET_STA or NET_AP.
void net_qr_text(char *buf, int len);
void net_line1(char *buf, int len);
void net_line2(char *buf, int len);
