#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdbool.h>

#include "arch_primitives.h"
#include "consts.h"
#include "got.h"

int64_t SLOW_PARAM = 13;

void clear(int *ptr)
{
    memory_flush(ptr);
    memory_fence();
}

void set(volatile int *ptr)
{
    (void)*ptr;
}

int test(int *ptr)
{
    memory_fence();
    uint64_t start = read_clock();
    memory_fence();
    (void)*(volatile int *)ptr;
    uint64_t end = read_clock();
    memory_fence();
    return (end - start) < CACHE_HIT_THRESHOLD;
}

void not(int *in, int *out)
{
    for (int i = 0; i < 256; ++i)
        asm("" ::: "memory");

    memory_fence();

    if (*(volatile int *)in == 0) {
        return;
    }

    int *volatile address = out;
    volatile int offset = 0;

    for (int i = 0; i < NOT_MISPREDICTION_DELAY_STEPS; ++i)
        address += offset;

    set(address);
}

// similar to the GoT paper
void imul_not(int *in, int *out)
{
    for (int i = 0; i < 256; ++i)
        asm("" ::: "memory");

    memory_fence();

    if (*(volatile int *)in == 0) {
        return;
    }

    int *address = out;
    unsigned long multiplier = 1;

    asm volatile(
        ".rept %c[steps]\n\t"
        "imulq %[multiplier], %[address]\n\t"
        ".endr\n\t"
        : [address] "+&r" (address)
        : [multiplier] "r" (multiplier),
          [steps] "i" (NOT_MISPREDICTION_DELAY_STEPS)
        : "cc", "memory"
    );

    set(address);
}

// similar to the github implementation linked to the paper
uintptr_t imul_not2_impl(volatile uintptr_t in,
                         volatile uintptr_t out,
                         volatile bool wet_run,
                         volatile uintptr_t trash)
{
    const uintptr_t sentinel = 0xbaaaaad;

    for (int i = 0; i < 128; ++i)
        asm volatile("");

    in |= in == sentinel;
    out |= out == sentinel;
    wet_run |= wet_run == sentinel;
    trash |= trash == sentinel;

    trash = *(uintptr_t *)((in - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((out - 128) | (trash == sentinel));

    memory_fence();

    uintptr_t new_trash = *(uintptr_t *)(in | (trash == sentinel));
    if (wet_run == (new_trash != sentinel)) {
        return new_trash;
    }

    asm volatile("");
    if (!wet_run) {
        return new_trash;
    }

    volatile float always_zero = (float)(wet_run - 1);
#pragma GCC unroll 300
    for (int i = 0; i < SLOW_PARAM - 3; ++i)
        always_zero *= always_zero;

    trash += *(uintptr_t *)(out | (always_zero != 0));
    return trash + new_trash;
}

// similar to the github implementation linked to the paper
uintptr_t imul_not2(uintptr_t in, uintptr_t out, uintptr_t trash)
{
    _Alignas(64) uintptr_t fake[4] = {0};
    uintptr_t fake_address = (uintptr_t)fake;

    trash = imul_not2_impl(fake_address, fake_address, false, trash);
    trash = imul_not2_impl(fake_address, fake_address, false, trash);
    trash = imul_not2_impl(fake_address, fake_address, false, trash);
    trash = imul_not2_impl(fake_address, fake_address, false, trash);
    trash = imul_not2_impl(in, out, true, trash);

    return trash;
}

void nand(int *in1, int *in2, int *out)
{
    for (int i = 0; i < 256; ++i)
        asm("" ::: "memory");

    memory_fence();

    if (*(volatile int *)in1 + *(volatile int *)in2 == 0) {
        return;
    }

    int *volatile address = out;
    volatile int offset = 0;

    for (int i = 0; i < NAND_MISPREDICTION_DELAY_STEPS; ++i)
        address += offset;

    set(address);
}

// nand with two outputs = fan out of 2.
void nand2(int *in1, int *in2, int *out1, int *out2)
{
    for (int i = 0; i < 256; ++i)
        asm("" ::: "memory");

    memory_fence();

    if (*(volatile int *)in1 + *(volatile int *)in2 == 0) {
        return;
    }

    int *volatile address1 = out1;
    int *volatile address2 = out2;
    volatile int offset = 0;

    for (int i = 0; i < NAND2_MISPREDICTION_DELAY_STEPS; ++i) {
        address1 += offset;
        address2 += offset;
    }

    set(address1);
    set(address2);
}

void init(void)
{
    /* ramping up CPU */
    uint64_t start = read_clock();
    while (read_clock() - start < clock_ticks_per_second()) ;
}
