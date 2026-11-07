#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

typedef enum { ORI_NONE = -1, ORI_L = 0, ORI_LF = 1, ORI_P = 2, ORI_PF = 3 } orient_t;

typedef enum { IMU_EV_NONE, IMU_EV_ORIENT, IMU_EV_KNOCK3, IMU_EV_MOTION } imu_ev_type_t;
typedef struct { imu_ev_type_t type; orient_t orient; } imu_event_t;

bool imu_init(void);
void imu_start(void);                   // 100 Hz sampling task
bool imu_get_event(imu_event_t *ev);    // non-blocking
orient_t imu_current(void);             // last stable orientation (ORI_NONE before the first)
bool imu_is_flat(void);
void imu_set_mapping(int map);          // 0..7, fixes axis/sign differences between boards
int  imu_mapping(void);
void imu_get_gravity(float g[3]);
void imu_set_calibration(const float ref[4][3], bool valid);   // per-position gravity from Settings > Direction
void imu_arm_wake_on_motion(void);      // before deep sleep: WoM on INT1 -> EXIO2 -> TCA INT
