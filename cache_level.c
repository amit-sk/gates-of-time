#include <stdint.h>

#include "cache_level.h"

#if defined(__x86_64__) || defined(_M_X64)

#include <x86intrin.h>

uint64_t cache_level_measure_ticks(const volatile int *ptr)
{
    uint32_t auxiliary;
    uint32_t value;

    _mm_lfence();
    uint64_t start = __rdtsc();
    _mm_lfence();
    asm volatile("movl (%1), %0"
                 : "=r"(value)
                 : "r"(ptr)
                 : "memory");
    uint64_t end = __rdtscp(&auxiliary);
    _mm_lfence();
    asm volatile("" : : "r"(value) : "memory");
    return end - start;
}

uint64_t cache_level_measure_pointer_ticks(const volatile uintptr_t *ptr,
                                           uintptr_t *value)
{
    uint32_t auxiliary;
    uintptr_t loaded_value;

    _mm_lfence();
    uint64_t start = __rdtsc();
    _mm_lfence();
    asm volatile("movq (%1), %0"
                 : "=r"(loaded_value)
                 : "r"(ptr)
                 : "memory");
    uint64_t end = __rdtscp(&auxiliary);
    _mm_lfence();
    *value = loaded_value;
    return end - start;
}

#else

#error "cache-level timing needs to be implemented for this architecture"

#endif
