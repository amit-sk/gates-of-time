#ifndef ARCH_PRIMITIVES_H
#define ARCH_PRIMITIVES_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * Burn a tuned amount of time by chaining `steps` dependent multiplies on
 * `address`. The multiplier is 1 so the value is preserved, but the compiler
 * cannot see that (without optimization), so each multiply must wait for the
 * previous result -- the chain length sets the latency. `steps` must be a
 * compile-time constant.
 */
__attribute__((always_inline)) static inline void *instructions_delay(void *address, int steps);

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

static inline void *instructions_delay(void *address, int steps)
{
    unsigned long multiplier = 1;

    asm volatile(
        ".rept %c[steps]\n\t"
        "imulq %[multiplier], %[address]\n\t"
        ".endr\n\t"
        : [address] "+&r" (address)
        : [multiplier] "r" (multiplier),
          [steps] "i" (steps)
        : "cc", "memory"
    );

    return address;
}

#elif defined(__aarch64__) || defined(__arm64__)

static inline void memory_flush(const void *ptr)
{
    asm volatile(
        "dc civac, %0\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(ptr)
        : "memory"
    );
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

static inline void *instructions_delay(void *address, int steps)
{
    unsigned long multiplier = 1;

    asm volatile(
        ".rept %c[steps]\n\t"
        "mul %[address], %[address], %[multiplier]\n\t"
        ".endr\n\t"
        : [address] "+&r" (address)
        : [multiplier] "r" (multiplier),
          [steps] "i" (steps)
        : "cc", "memory"
    );

    return address;
}

#else

#error "Unsupported architecture"

#endif

#endif /* ARCH_PRIMITIVES_H */
