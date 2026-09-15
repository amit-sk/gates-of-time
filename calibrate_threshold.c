#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch_primitives.h"

#define SAMPLES 10000

static uint64_t cached[SAMPLES];
static uint64_t flushed[SAMPLES];

static uint64_t measure(int *ptr)
{
    memory_fence();
    uint64_t start = read_clock();
    memory_fence();
    (void)*(volatile int *)ptr;
    uint64_t end = read_clock();
    memory_fence();
    return end - start;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    _Alignas(64) int probe[16] = {0};

    for (int i = 0; i < 1000; ++i)
        (void)measure(probe);

    for (int i = 0; i < SAMPLES; ++i) {
        (void)*(volatile int *)probe;
        cached[i] = measure(probe);

        memory_flush(probe);
        memory_fence();
        flushed[i] = measure(probe);
    }

    qsort(cached, SAMPLES, sizeof(cached[0]), compare);
    qsort(flushed, SAMPLES, sizeof(flushed[0]), compare);

    printf("Cached:  median=%" PRIu64 ", p95=%" PRIu64 " ticks\n",
           cached[SAMPLES / 2], cached[SAMPLES * 95 / 100]);
    printf("Flushed: p05=%" PRIu64 ", median=%" PRIu64 " ticks\n",
           flushed[SAMPLES * 5 / 100], flushed[SAMPLES / 2]);

    uint64_t low = cached[SAMPLES * 95 / 100];
    uint64_t high = flushed[SAMPLES * 5 / 100];
    if (low >= high) {
        puts("Distributions overlap; using the midpoint between medians.");
        low = cached[SAMPLES / 2];
        high = flushed[SAMPLES / 2];
    }
    if (low >= high) {
        fputs("No usable timing separation; repeat calibration.\n", stderr);
        return EXIT_FAILURE;
    }

    uint64_t threshold = low + (high - low) / 2;
    int false_misses = 0;
    int false_hits = 0;
    for (int i = 0; i < SAMPLES; ++i) {
        false_misses += cached[i] >= threshold;
        false_hits += flushed[i] < threshold;
    }

    printf("#define CACHE_HIT_THRESHOLD (%" PRIu64 ")\n", threshold);
    printf("With elapsed < threshold: cached errors=%.2f%%, flushed errors=%.2f%%\n",
           100.0 * false_misses / SAMPLES, 100.0 * false_hits / SAMPLES);
    return EXIT_SUCCESS;
}
