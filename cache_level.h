#ifndef CACHE_LEVEL_H
#define CACHE_LEVEL_H

#include <stdint.h>

uint64_t cache_level_measure_ticks(const volatile int *ptr);
uint64_t cache_level_measure_pointer_ticks(const volatile uintptr_t *ptr,
                                           uintptr_t *value);

#endif
