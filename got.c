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
#include <float.h>

#include "arch_primitives.h"
#include "consts.h"
#include "got.h"

#define PAGE_BYTES 4096
#define ADDRESS_PADDING_BYTES 128
#define SIGNAL_STRIDE_BYTES 320

int64_t SLOW_PARAM = 13;
static int predictor_training_input = 1;

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

uintptr_t fan2_impl(uintptr_t in,
                    uintptr_t out1,
                    uintptr_t out2,
                    bool wet_run,
                    uintptr_t trash)
{
    for (int i = 0; i < 256; ++i) {
        asm("" ::: "memory");
    }

    const uintptr_t sentinel = 0xbaaaaad;

    in |= in == sentinel;
    out1 |= out1 == sentinel;
    out2 |= out2 == sentinel;
    wet_run |= wet_run == sentinel;
    trash |= trash == sentinel;

    trash = *(uintptr_t *)((in - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((out1 - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((out2 - 128) | (trash == sentinel));
    memory_fence();

    const double start = DBL_MIN;
    const double denormal_result = DBL_MIN / 2;
    double divide_by = wet_run + 1;
    double result = start / divide_by;

    if (result == denormal_result) {
        return trash;
    }

    asm volatile("");
    if (!wet_run) {
        return trash;
    }

    trash = *(uintptr_t *)in;

    volatile uintptr_t address1 = out1 + trash;
    volatile uintptr_t address2 = out2 + trash;
    volatile uintptr_t offset = 0;

    for (int i = 0; i < FAN2_MISPREDICTION_DELAY_STEPS; ++i) {
        address1 += offset;
        address2 += offset;
    }

    uintptr_t sum = 0;
    sum += *(uintptr_t *)address1;
    sum += *(uintptr_t *)address2;
    return trash + sum;
}

uintptr_t fan2(uintptr_t in, uintptr_t out1, uintptr_t out2,
               uintptr_t trash)
{
    _Alignas(64) unsigned char fake_storage[256] = {0};
    uintptr_t fake = (uintptr_t)(fake_storage + 128);

    trash = fan2_impl(fake, fake, fake, false, trash);
    trash = fan2_impl(fake, fake, fake, false, trash);
    trash = fan2_impl(fake, fake, fake, false, trash);
    trash = fan2_impl(fake, fake, fake, false, trash);
    return fan2_impl(in, out1, out2, true, trash);
}

static uintptr_t and_gate_impl(uintptr_t input1,
                               uintptr_t input2,
                               uintptr_t output,
                               bool wet_run,
                               uintptr_t trash)
{
    for (int i = 0; i < 256; ++i) {
        asm("" ::: "memory");
    }

    const uintptr_t sentinel = 0xbaaaaad;

    input1 |= input1 == sentinel;
    input2 |= input2 == sentinel;
    output |= output == sentinel;
    wet_run |= wet_run == sentinel;
    trash |= trash == sentinel;

    trash = *(uintptr_t *)((input1 - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((input2 - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((output - 128) | (trash == sentinel));
    memory_fence();

    const double start = DBL_MIN;
    const double denormal_result = DBL_MIN / 2;
    double divide_by = wet_run + 1;
    double result = start / divide_by;

    if (result == denormal_result) {
        return trash;
    }

    asm volatile("");
    if (!wet_run) {
        return trash;
    }

    uintptr_t value1 = *(uintptr_t *)(input1 | (trash == sentinel));
    uintptr_t value2 = *(uintptr_t *)(input2 | (trash == sentinel));
    trash = value1 | value2;

    volatile uintptr_t address = output + trash;
    volatile uintptr_t offset = 0;

    for (int i = 0; i < AND_MISPREDICTION_DELAY_STEPS; ++i) {
        address += offset;
    }

    uintptr_t sum = *(uintptr_t *)address;
    return trash + sum;
}

uintptr_t and(uintptr_t input1, uintptr_t input2, uintptr_t output, uintptr_t trash)
{
    _Alignas(64) unsigned char fake_storage[256] = {0};
    uintptr_t fake = (uintptr_t)(fake_storage + 128);

    trash = and_gate_impl(fake, fake, fake, false, trash);
    trash = and_gate_impl(fake, fake, fake, false, trash);
    trash = and_gate_impl(fake, fake, fake, false, trash);
    trash = and_gate_impl(fake, fake, fake, false, trash);
    return and_gate_impl(input1, input2, output, true, trash);
}

static uintptr_t or_gate_impl(uintptr_t input1,
                              uintptr_t input2,
                              uintptr_t output,
                              bool wet_run,
                              uintptr_t trash)
{
    for (int i = 0; i < 256; ++i) {
        asm("" ::: "memory");
    }

    const uintptr_t sentinel = 0xbaaaaad;

    input1 |= input1 == sentinel;
    input2 |= input2 == sentinel;
    output |= output == sentinel;
    wet_run |= wet_run == sentinel;
    trash |= trash == sentinel;

    trash = *(uintptr_t *)((input1 - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((input2 - 128) | (trash == sentinel));
    trash = *(uintptr_t *)((output - 128) | (trash == sentinel));
    memory_fence();

    const double start = DBL_MIN;
    const double denormal_result = DBL_MIN / 2;
    double divide_by = wet_run + 1;
    double result = start / divide_by;

    if (result == denormal_result) {
        return trash;
    }

    asm volatile("");
    if (!wet_run) {
        return trash;
    }

    uintptr_t value1 = *(uintptr_t *)(input1 | (trash == sentinel));
    uintptr_t value2 = *(uintptr_t *)(input2 | (trash == sentinel));
    volatile uintptr_t address1 = output + value1;
    volatile uintptr_t address2 = output + value2;
    volatile uintptr_t offset = 0;

    for (int i = 0; i < OR_MISPREDICTION_DELAY_STEPS; ++i) {
        address1 += offset;
        address2 += offset;
    }

    uintptr_t sum = 0;
    sum += *(uintptr_t *)address1;
    sum += *(uintptr_t *)address2;
    return trash + value1 + value2 + sum;
}

uintptr_t or(uintptr_t input1, uintptr_t input2, uintptr_t output,
                  uintptr_t trash)
{
    _Alignas(64) unsigned char fake_storage[256] = {0};
    uintptr_t fake = (uintptr_t)(fake_storage + 128);

    trash = or_gate_impl(fake, fake, fake, false, trash);
    trash = or_gate_impl(fake, fake, fake, false, trash);
    trash = or_gate_impl(fake, fake, fake, false, trash);
    trash = or_gate_impl(fake, fake, fake, false, trash);
    return or_gate_impl(input1, input2, output, true, trash);
}

// similar to the github implementation linked to the paper
uintptr_t half_adder_impl(volatile uintptr_t a, volatile uintptr_t b, volatile uintptr_t sum, volatile uintptr_t carry, volatile uintptr_t trash)
{
    enum {
        A1,
        A2,
        B1,
        B2,
        N1,
        N2,
        O,
        TRAINING_OUTPUT1,
        TRAINING_OUTPUT2,
        TEMPORARY_COUNT
    };

    void *allocation = aligned_alloc(PAGE_BYTES, PAGE_BYTES);
    if (allocation == NULL) {
        return trash;
    }

    memset(allocation, 0, PAGE_BYTES);

    uintptr_t temporary[TEMPORARY_COUNT];
    for (int i = 0; i < TEMPORARY_COUNT; ++i) {
        temporary[i] = (uintptr_t)((char *)allocation
                                   + ADDRESS_PADDING_BYTES
                                   + i * SIGNAL_STRIDE_BYTES);
    }

    /*
     * a1,a2 = FAN(a)
     * b1,b2 = FAN(b)
     * n1, n2 = NAND2(a1, b1)
     * o = OR(a2, b2)
     * sum = AND(n1, o)
     * carry = NOT(n2)
    */
    memory_flush((void *)temporary[A1]);
    memory_flush((void *)temporary[A2]);
    memory_fence();

    trash = fan2(a, temporary[A1], temporary[A2], trash);

    memory_flush((void *)temporary[B1]);
    memory_flush((void *)temporary[B2]);
    memory_fence();

    trash = fan2(b, temporary[B1], temporary[B2], trash);

    memory_fence();

    memory_flush((void *)temporary[N1]);
    memory_flush((void *)temporary[N2]);
    memory_fence();

    nand2(&predictor_training_input, &predictor_training_input,
          (int *)temporary[TRAINING_OUTPUT1],
          (int *)temporary[TRAINING_OUTPUT2]);
    nand2(&predictor_training_input, &predictor_training_input,
          (int *)temporary[TRAINING_OUTPUT1],
          (int *)temporary[TRAINING_OUTPUT2]);
    nand2(&predictor_training_input, &predictor_training_input,
          (int *)temporary[TRAINING_OUTPUT1],
          (int *)temporary[TRAINING_OUTPUT2]);
    nand2(&predictor_training_input, &predictor_training_input,
          (int *)temporary[TRAINING_OUTPUT1],
          (int *)temporary[TRAINING_OUTPUT2]);
    nand2((int *)temporary[A1], (int *)temporary[B1],
          (int *)temporary[N1], (int *)temporary[N2]);

    memory_flush((void *)temporary[O]);
    memory_fence();

    trash = or(temporary[A2], temporary[B2], temporary[O], trash);

    memory_flush((void *)sum);
    memory_fence();

    trash = and(temporary[N1], temporary[O], sum, trash);

    memory_flush((void *)carry);
    memory_fence();

    not(&predictor_training_input, (int *)temporary[TRAINING_OUTPUT1]);
    not(&predictor_training_input, (int *)temporary[TRAINING_OUTPUT1]);
    not(&predictor_training_input, (int *)temporary[TRAINING_OUTPUT1]);
    not(&predictor_training_input, (int *)temporary[TRAINING_OUTPUT1]);
    not((int *)temporary[N2], (int *)carry);

    memory_fence();

    free(allocation);
    return trash;
}

void init(void)
{
    /* ramping up CPU */
    uint64_t start = read_clock();
    while (read_clock() - start < clock_ticks_per_second()) ;
}
