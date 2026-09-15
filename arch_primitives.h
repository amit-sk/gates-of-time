#ifndef ARCH_PRIMITIVES_H
#define ARCH_PRIMITIVES_H

#include <stdatomic.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)

#include <x86intrin.h>

static inline uint64_t read_clock(void)
{
    uint32_t dummy;
    return __rdtscp(&dummy);
}

static inline void memory_fence(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

static inline void memory_flush(const void *ptr)
{
    _mm_clflush(ptr);
}

static inline uint64_t clock_ticks_per_second(void)
{
    return 1000000000ULL;
}

#elif defined(__aarch64__) || defined(__arm64__)

static inline void memory_flush(const void *ptr)
{
#error "memory_flush needs to be implemented for aarch64"
}

static inline uint64_t read_clock(void)
{
    uint64_t t;

    asm volatile(
        "isb\n"
        "mrs %0, cntvct_el0\n"
        "isb\n"
        : "=r"(t)
        :
        : "memory"
    );

    return t;
}

static inline void memory_fence(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

static inline uint64_t clock_ticks_per_second(void)
{
    uint64_t frequency;

    asm volatile(
        "isb\n"
        "mrs %0, cntfrq_el0\n"
        "isb\n"
        : "=r"(frequency)
        :
        : "memory"
    );

    return frequency;
}

#else

#error "Unsupported architecture"

#endif

#endif /* ARCH_PRIMITIVES_H */
