#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
#define xSemaphoreTakeRecursive(s, t) pdTRUE
#define xSemaphoreGiveRecursive(s) pdTRUE
#define xSemaphoreTake(s, t) pdTRUE
#define xSemaphoreGive(s) pdTRUE
