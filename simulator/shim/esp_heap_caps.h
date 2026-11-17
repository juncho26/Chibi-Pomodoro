#pragma once
// simulator stand-in: plenty of memory, nothing to watch
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_DMA (1 << 3)
static inline size_t heap_caps_get_free_size(uint32_t c) { (void)c; return 1 << 20; }
static inline size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 1 << 20; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t c) { (void)c; return 1 << 20; }
