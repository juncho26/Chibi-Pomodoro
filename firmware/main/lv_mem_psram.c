// LVGL keeps its objects, glyph caches and sprites in PSRAM (the 8 MB external RAM).
// With the default allocator, everything under 4 KB went into the chip's small internal RAM,
// which Wi-Fi and the web page server also need: the cached digits and rain sprites alone used
// most of it, and the phone page server could no longer start. Falls back to internal RAM only
// if PSRAM is ever full.
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, new_size);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
