#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch_primitives.h"
#include "consts.h"
#include "got.h"

#define CACHE_SAMPLE_COUNT 10000
#define CACHE_WARMUP_READ_COUNT 1000
#define DELAY_TRIAL_COUNT 1000
#define MAX_DELAY_STEPS 64
#define PAGE_BYTES 4096
#define ADDRESS_OFFSET_BYTES 128

static uint64_t cached_timings[CACHE_SAMPLE_COUNT];
static uint64_t uncached_timings[CACHE_SAMPLE_COUNT];

struct delay_result {
    int steps;
    int cached_input_correct;
    int uncached_input_correct;
};

struct nand2_delay_result {
    int correct[2][2];
};

struct nand2_calibration_memory {
    void *input1_allocation;
    void *input2_allocation;
    void *output1_allocation;
    void *output2_allocation;
    void *training_output1_allocation;
    void *training_output2_allocation;
    int *input1;
    int *input2;
    int *output1;
    int *output2;
    int *training_output1;
    int *training_output2;
};

static uint64_t measure_access_ticks(int *address)
{
    memory_fence();
    uint64_t start_ticks = read_clock();
    memory_fence();
    (void)*(volatile int *)address;
    uint64_t end_ticks = read_clock();
    memory_fence();
    return end_ticks - start_ticks;
}

static int compare_timings(const void *left, const void *right)
{
    uint64_t left_ticks = *(const uint64_t *)left;
    uint64_t right_ticks = *(const uint64_t *)right;
    return (left_ticks > right_ticks) - (left_ticks < right_ticks);
}

static void collect_cache_timings(int *probe)
{
    for (int sample = 0; sample < CACHE_WARMUP_READ_COUNT; ++sample)
        (void)measure_access_ticks(probe);

    for (int sample = 0; sample < CACHE_SAMPLE_COUNT; ++sample) {
        (void)*(volatile int *)probe;
        cached_timings[sample] = measure_access_ticks(probe);

        memory_flush(probe);
        memory_fence();
        uncached_timings[sample] = measure_access_ticks(probe);
    }

    qsort(cached_timings, CACHE_SAMPLE_COUNT, sizeof(cached_timings[0]), compare_timings);
    qsort(uncached_timings, CACHE_SAMPLE_COUNT, sizeof(uncached_timings[0]), compare_timings);
}

static void report_cache_timings(void)
{
    printf("Cached:  median=%" PRIu64 ", p95=%" PRIu64 " ticks\n",
           cached_timings[CACHE_SAMPLE_COUNT / 2],
           cached_timings[CACHE_SAMPLE_COUNT * 95 / 100]);
    printf("Flushed: p05=%" PRIu64 ", median=%" PRIu64 " ticks\n",
           uncached_timings[CACHE_SAMPLE_COUNT * 5 / 100],
           uncached_timings[CACHE_SAMPLE_COUNT / 2]);
}

static int select_cache_hit_threshold(uint64_t *cache_hit_threshold)
{
    uint64_t cached_upper_ticks = cached_timings[CACHE_SAMPLE_COUNT * 95 / 100];
    uint64_t uncached_lower_ticks = uncached_timings[CACHE_SAMPLE_COUNT * 5 / 100];

    if (cached_upper_ticks >= uncached_lower_ticks) {
        puts("Timing overlap; using median midpoint.");
        cached_upper_ticks = cached_timings[CACHE_SAMPLE_COUNT / 2];
        uncached_lower_ticks = uncached_timings[CACHE_SAMPLE_COUNT / 2];
    }
    if (cached_upper_ticks >= uncached_lower_ticks) {
        fputs("No timing separation; rerun calibration.\n", stderr);
        return EXIT_FAILURE;
    }

    *cache_hit_threshold = cached_upper_ticks
                        + (uncached_lower_ticks - cached_upper_ticks) / 2;
    return EXIT_SUCCESS;
}

static void report_cache_threshold(uint64_t cache_hit_threshold)
{
    int cached_error_count = 0;
    int uncached_error_count = 0;

    for (int sample = 0; sample < CACHE_SAMPLE_COUNT; ++sample) {
        cached_error_count += cached_timings[sample] >= cache_hit_threshold;
        uncached_error_count += uncached_timings[sample] < cache_hit_threshold;
    }

    printf("Cache errors: cached=%.2f%%, flushed=%.2f%%\n",
           100.0 * cached_error_count / CACHE_SAMPLE_COUNT,
           100.0 * uncached_error_count / CACHE_SAMPLE_COUNT);
    printf("#define CACHE_HIT_THRESHOLD (%" PRIu64 ")\n", cache_hit_threshold);
}

static int calibrate_cache_hit_threshold(int *probe, uint64_t *cache_hit_threshold)
{
    collect_cache_timings(probe);

    puts("=== Cache hit threshold calibration ===");
    report_cache_timings();
    if (select_cache_hit_threshold(cache_hit_threshold) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    report_cache_threshold(*cache_hit_threshold);
    return EXIT_SUCCESS;
}

static void prepare_branch_history(void)
{
    for (volatile int i = 0; i < 256; ++i) ;
}

static void not_with_delay(int *input, int *output, int delay_steps)
{
    prepare_branch_history();
    if (*(volatile int *)input == 0)
        return;

    int *volatile output_address = output;
    volatile int offset = 0;
    for (int step = 0; step < delay_steps; ++step)
        output_address += offset;
    (void)*(volatile int *)output_address;
}

static struct delay_result measure_delay(int *input, int *output, int delay_steps,
                                        uint64_t cache_hit_threshold,
                                        void (*candidate_gate)(int *, int *, int))
{
    static int training_condition = 1;
    struct delay_result result = {.steps = delay_steps};
    void (*volatile gate)(int *, int *, int) = candidate_gate;

    for (int trial = 0; trial < DELAY_TRIAL_COUNT; ++trial) {
        for (int input_cached = 0; input_cached < 2; ++input_cached) {
            gate(&training_condition, output, delay_steps);
            gate(&training_condition, output, delay_steps);

            memory_flush(input);
            memory_flush(output);
            memory_fence();
            if (input_cached)
                (void)*(volatile int *)input;
            memory_fence();

            gate(input, output, delay_steps);
            int output_cached = measure_access_ticks(output) < cache_hit_threshold;
            if (input_cached)
                result.cached_input_correct += !output_cached;
            else
                result.uncached_input_correct += output_cached;
        }
    }

    return result;
}

static int minimum_correct_count(struct delay_result result)
{
    return result.cached_input_correct < result.uncached_input_correct
         ? result.cached_input_correct : result.uncached_input_correct;
}

static struct delay_result find_best_delay(int *input, int *output,
                                           uint64_t cache_hit_threshold)
{
    struct delay_result best_result = {0};
    int best_minimum_correct = -1;
    int best_total_correct = -1;

    for (int delay_steps = 0; delay_steps <= MAX_DELAY_STEPS; ++delay_steps) {
        struct delay_result candidate_result = measure_delay(
            input, output, delay_steps, cache_hit_threshold, not_with_delay);
        int candidate_minimum_correct = minimum_correct_count(candidate_result);
        int candidate_total_correct = candidate_result.cached_input_correct
                                    + candidate_result.uncached_input_correct;

        if (candidate_minimum_correct > best_minimum_correct ||
            (candidate_minimum_correct == best_minimum_correct &&
             candidate_total_correct > best_total_correct)) {
            best_result = candidate_result;
            best_minimum_correct = candidate_minimum_correct;
            best_total_correct = candidate_total_correct;
        }
    }

    return best_result;
}

static void report_best_delay(struct delay_result best_result)
{
    printf("NOT delay best results (%d steps): accuracy cached=%.2f%%, uncached=%.2f%%\n",
           best_result.steps,
           100.0 * best_result.cached_input_correct / DELAY_TRIAL_COUNT,
           100.0 * best_result.uncached_input_correct / DELAY_TRIAL_COUNT);
}

static void report_delay_calibration(struct delay_result best_result)
{
    report_best_delay(best_result);
    printf("#define NOT_MISPREDICTION_DELAY_STEPS (%d)\n", best_result.steps);
}

static int calibrate_misprediction_delay(int *output, uint64_t cache_hit_threshold)
{
    puts("\n=== Misprediction delay steps calibration ===");
    int *input = calloc(1, sizeof(*input));
    if (input == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    struct delay_result best_result = find_best_delay(input, output,
                                                     cache_hit_threshold);
    free(input);

    report_delay_calibration(best_result);
    return EXIT_SUCCESS;
}

static int allocate_calibration_line(void **allocation, int **address)
{
    *allocation = aligned_alloc(PAGE_BYTES, PAGE_BYTES);
    if (*allocation == NULL) {
        return 0;
    }

    memset(*allocation, 0, PAGE_BYTES);
    *address = (int *)((char *)*allocation + ADDRESS_OFFSET_BYTES);
    return 1;
}

static int allocate_nand2_calibration_memory(
    struct nand2_calibration_memory *memory)
{
    if (!allocate_calibration_line(&memory->input1_allocation,
                                   &memory->input1)
        || !allocate_calibration_line(&memory->input2_allocation,
                                      &memory->input2)
        || !allocate_calibration_line(&memory->output1_allocation,
                                      &memory->output1)
        || !allocate_calibration_line(&memory->output2_allocation,
                                      &memory->output2)
        || !allocate_calibration_line(&memory->training_output1_allocation,
                                      &memory->training_output1)
        || !allocate_calibration_line(&memory->training_output2_allocation,
                                      &memory->training_output2)) {
        return 0;
    }

    return 1;
}

static void free_nand2_calibration_memory(
    struct nand2_calibration_memory *memory)
{
    free(memory->input1_allocation);
    free(memory->input2_allocation);
    free(memory->output1_allocation);
    free(memory->output2_allocation);
    free(memory->training_output1_allocation);
    free(memory->training_output2_allocation);
}

static void run_nand2_with_training(
    struct nand2_calibration_memory *memory)
{
    static int training_input = 1;

    nand2(&training_input, &training_input, memory->training_output1,
          memory->training_output2);
    nand2(&training_input, &training_input, memory->training_output1,
          memory->training_output2);
    nand2(&training_input, &training_input, memory->training_output1,
          memory->training_output2);
    nand2(&training_input, &training_input, memory->training_output1,
          memory->training_output2);
    nand2(memory->input1, memory->input2, memory->output1, memory->output2);
}

static struct nand2_delay_result measure_nand2_delay(
    struct nand2_calibration_memory *memory,
    uint64_t cache_hit_threshold)
{
    struct nand2_delay_result result = {0};

    for (int trial = 0; trial < DELAY_TRIAL_COUNT; ++trial) {
        for (int input1_cached = 0; input1_cached < 2; ++input1_cached) {
            for (int input2_cached = 0; input2_cached < 2; ++input2_cached) {
                clear(memory->output1);
                clear(memory->output2);
                clear(memory->input1);
                clear(memory->input2);

                if (input1_cached) {
                    set(memory->input1);
                }
                if (input2_cached) {
                    set(memory->input2);
                }
                memory_fence();

                run_nand2_with_training(memory);

                int output1_cached = measure_access_ticks(memory->output1)
                                   < cache_hit_threshold;
                int output2_cached = measure_access_ticks(memory->output2)
                                   < cache_hit_threshold;
                int expected_output = !(input1_cached && input2_cached);

                result.correct[input1_cached][input2_cached] +=
                    output1_cached == expected_output
                    && output2_cached == expected_output;
            }
        }
    }

    return result;
}

static void report_nand2_delay(struct nand2_delay_result result)
{
    printf("NAND2 delay results (%d steps): joint accuracy "
           "uncached/uncached=%.2f%%, uncached/cached=%.2f%%, "
           "cached/uncached=%.2f%%, cached/cached=%.2f%%\n",
           NAND2_MISPREDICTION_DELAY_STEPS,
           100.0 * result.correct[0][0] / DELAY_TRIAL_COUNT,
           100.0 * result.correct[0][1] / DELAY_TRIAL_COUNT,
           100.0 * result.correct[1][0] / DELAY_TRIAL_COUNT,
           100.0 * result.correct[1][1] / DELAY_TRIAL_COUNT);
    printf("#define NAND2_MISPREDICTION_DELAY_STEPS (%d)\n",
           NAND2_MISPREDICTION_DELAY_STEPS);
}

static int calibrate_nand2_delay(uint64_t cache_hit_threshold)
{
    int return_code = EXIT_FAILURE;
    struct nand2_calibration_memory memory = {0};

    puts("\n=== NAND2 delay steps calibration ===");
    if (!allocate_nand2_calibration_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    struct nand2_delay_result result = measure_nand2_delay(
        &memory, cache_hit_threshold);
    report_nand2_delay(result);
    return_code = EXIT_SUCCESS;

cleanup:
    free_nand2_calibration_memory(&memory);
    return return_code;
}

int main(void)
{
    _Alignas(64) int probe[16] = {0};
    uint64_t cache_hit_threshold;
    int return_code = EXIT_SUCCESS;

    init();
    if (calibrate_cache_hit_threshold(probe, &cache_hit_threshold)
        != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    if (calibrate_misprediction_delay(probe, cache_hit_threshold)
        != EXIT_SUCCESS) {
        return_code = EXIT_FAILURE;
    }
    if (calibrate_nand2_delay(cache_hit_threshold) != EXIT_SUCCESS) {
        return_code = EXIT_FAILURE;
    }

    return return_code;
}
