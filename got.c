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

#include "arch_primitives.h"
#include "consts.h"
#include "got.h"

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
    if (*(volatile int *)in == 0) {
        return;
    }

    int *volatile address = out;
    volatile int offset = 0;

    for (int i = 0; i < MISPREDICTION_DELAY_STEPS; ++i)
        address += offset;

    set(address);
}

void init(void)
{
    /* ramping up CPU */
    uint64_t start = read_clock();
    while (read_clock() - start < clock_ticks_per_second()) ;
}
