#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch_primitives.h"
#include "consts.h"
#include "got.h"

#define CACHE_SAMPLE_COUNT 10000
#define CACHE_WARMUP_READ_COUNT 1000
#define DELAY_TRIAL_COUNT 1000
#define MAX_DELAY_STEPS 64
#define REQUIRED_DELAY_ACCURACY_PERCENT 95

static uint64_t cached_timings[CACHE_SAMPLE_COUNT];
static uint64_t uncached_timings[CACHE_SAMPLE_COUNT];

struct delay_result {
    int steps;
    int cached_input_correct;
    int uncached_input_correct;
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

static void not_with_configured_delay(int *input, int *output, int delay_steps)
{
    (void)delay_steps;
    prepare_branch_history();
    not(input, output);
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

static int delay_is_reliable(struct delay_result result)
{
    return minimum_correct_count(result)
        >= DELAY_TRIAL_COUNT * REQUIRED_DELAY_ACCURACY_PERCENT / 100;
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

static int report_delay_calibration(struct delay_result best_result,
                                     struct delay_result configured_result)
{
    report_best_delay(best_result);

    int configured_delay_is_reliable = delay_is_reliable(configured_result);
    int selected_delay_steps = configured_delay_is_reliable
                             ? configured_result.steps : best_result.steps;
    printf("#define MISPREDICTION_DELAY_STEPS (%d)\n", selected_delay_steps);

    if (configured_delay_is_reliable)
        return EXIT_SUCCESS;

    if (!delay_is_reliable(best_result)) {
        printf("No delay reached %d%% accuracy for both inputs.\n",
               REQUIRED_DELAY_ACCURACY_PERCENT);
        return EXIT_FAILURE;
    }

    puts("Recompile and rerun to validate the delay.");
    return EXIT_SUCCESS;
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
    struct delay_result configured_result = measure_delay(
        input, output, MISPREDICTION_DELAY_STEPS, cache_hit_threshold,
        not_with_configured_delay);
    free(input);

    return report_delay_calibration(best_result, configured_result);
}

int main(void)
{
    _Alignas(64) int probe[16] = {0};
    uint64_t cache_hit_threshold;

    init();
    if (calibrate_cache_hit_threshold(probe, &cache_hit_threshold) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return calibrate_misprediction_delay(probe, cache_hit_threshold);
}
