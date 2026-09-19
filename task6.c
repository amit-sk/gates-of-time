#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arch_primitives.h"
#include "got.h"

#define TRIALS 100000
#define WARMUP_TRIALS 1000
#define PAGE_BYTES 4096
#define ADDRESS_PADDING_BYTES 128

struct cache_line {
    void *allocation;
    int *address;
};

struct fan2_memory {
    struct cache_line input;
    struct cache_line output1;
    struct cache_line output2;
};

struct fan2_accuracy_results {
    int totals[2];
    int output1_correct[2];
    int output2_correct[2];
    int both_correct[2];
    int outputs_equal[2];
};

struct observed_outputs {
    int output1_cached;
    int output2_cached;
};

struct and_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line output;
};

struct and_accuracy_results {
    int totals[2][2];
    int correct[2][2];
};

static int allocate_cache_line(struct cache_line *line)
{
    line->allocation = aligned_alloc(PAGE_BYTES, PAGE_BYTES);
    if (line->allocation == NULL) {
        return 0;
    }

    memset(line->allocation, 0, PAGE_BYTES);
    line->address = (int *)((char *)line->allocation + ADDRESS_PADDING_BYTES);
    return 1;
}

static int allocate_fan2_memory(struct fan2_memory *memory)
{
    if (!allocate_cache_line(&memory->input)
        || !allocate_cache_line(&memory->output1)
        || !allocate_cache_line(&memory->output2)) {
        return 0;
    }

    return 1;
}

static void free_cache_line(struct cache_line *line)
{
    free(line->allocation);
}

static void free_fan2_memory(struct fan2_memory *memory)
{
    free_cache_line(&memory->input);
    free_cache_line(&memory->output1);
    free_cache_line(&memory->output2);
}

static uintptr_t run_fan2_trial(struct fan2_memory *memory, int input_cached,
                                uintptr_t trash,
                                struct observed_outputs *outputs)
{
    clear(memory->input.address);
    clear(memory->output1.address);
    clear(memory->output2.address);

    if (input_cached) {
        set(memory->input.address);
    }
    memory_fence();

    trash = fan2((uintptr_t)memory->input.address,
                 (uintptr_t)memory->output1.address,
                 (uintptr_t)memory->output2.address, trash);
    outputs->output1_cached = test(memory->output1.address);
    outputs->output2_cached = test(memory->output2.address);
    return trash;
}

static void record_fan2_result(struct fan2_accuracy_results *results,
                               int input_cached,
                               const struct observed_outputs *outputs)
{
    int output1_correct = outputs->output1_cached == input_cached;
    int output2_correct = outputs->output2_cached == input_cached;

    ++results->totals[input_cached];
    results->output1_correct[input_cached] += output1_correct;
    results->output2_correct[input_cached] += output2_correct;
    results->both_correct[input_cached] += output1_correct && output2_correct;
    results->outputs_equal[input_cached] +=
        outputs->output1_cached == outputs->output2_cached;
}

static void print_rate(const char *label, int correct, int total)
{
    printf("  %s: %.2f%% (%d/%d)\n",
           label, 100.0 * correct / total, correct, total);
}

static void report_fan2_accuracy(const struct fan2_accuracy_results *results,
                                 int input_cached)
{
    int total = results->totals[input_cached];

    printf("%s input, expected outputs %s:\n",
           input_cached ? "Cached" : "Uncached",
           input_cached ? "cached" : "uncached");
    print_rate("Output 1 correct", results->output1_correct[input_cached], total);
    print_rate("Output 2 correct", results->output2_correct[input_cached], total);
    print_rate("Both correct", results->both_correct[input_cached], total);
    print_rate("Outputs identical", results->outputs_equal[input_cached], total);
}

static int test_fan2(void)
{
    int return_code = EXIT_FAILURE;
    struct fan2_memory memory = {0};
    struct fan2_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_fan2_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        struct observed_outputs outputs;
        trash = run_fan2_trial(&memory, rand() % 2, trash, &outputs);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input_cached = rand() % 2;
        struct observed_outputs outputs;

        trash = run_fan2_trial(&memory, input_cached, trash, &outputs);
        record_fan2_result(&results, input_cached, &outputs);
    }

    printf("FAN2 gate:\n");
    report_fan2_accuracy(&results, 0);
    report_fan2_accuracy(&results, 1);
    return_code = EXIT_SUCCESS;

cleanup:
    free_fan2_memory(&memory);
    return return_code;
}

static int allocate_and_memory(struct and_memory *memory)
{
    if (!allocate_cache_line(&memory->input1)
        || !allocate_cache_line(&memory->input2)
        || !allocate_cache_line(&memory->output)) {
        return 0;
    }

    return 1;
}

static void free_and_memory(struct and_memory *memory)
{
    free_cache_line(&memory->input1);
    free_cache_line(&memory->input2);
    free_cache_line(&memory->output);
}

static uintptr_t run_and_trial(struct and_memory *memory,
                               int input1_cached,
                               int input2_cached,
                               uintptr_t trash,
                               int *output_cached)
{
    clear(memory->input1.address);
    clear(memory->input2.address);
    clear(memory->output.address);

    if (input1_cached) {
        set(memory->input1.address);
    }
    if (input2_cached) {
        set(memory->input2.address);
    }
    memory_fence();

    trash = and_gate((uintptr_t)memory->input1.address,
                     (uintptr_t)memory->input2.address,
                     (uintptr_t)memory->output.address, trash);
    *output_cached = test(memory->output.address);
    return trash;
}

static void record_and_result(struct and_accuracy_results *results,
                              int input1_cached,
                              int input2_cached,
                              int output_cached)
{
    int expected_output = input1_cached && input2_cached;

    ++results->totals[input1_cached][input2_cached];
    results->correct[input1_cached][input2_cached] +=
        output_cached == expected_output;
}

static void report_and_accuracy(const struct and_accuracy_results *results,
                                int input1_cached,
                                int input2_cached)
{
    int total = results->totals[input1_cached][input2_cached];
    int correct = results->correct[input1_cached][input2_cached];
    int expected_output = input1_cached && input2_cached;

    printf("Inputs (%s, %s), expected output %s: %.2f%% (%d/%d)\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           expected_output ? "cached" : "uncached",
           100.0 * correct / total, correct, total);
}

static int test_and_gate(void)
{
    int return_code = EXIT_FAILURE;
    struct and_memory memory = {0};
    struct and_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_and_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        int output_cached;
        trash = run_and_trial(&memory, rand() % 2, rand() % 2, trash,
                              &output_cached);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        int output_cached;

        trash = run_and_trial(&memory, input1_cached, input2_cached, trash,
                              &output_cached);
        record_and_result(&results, input1_cached, input2_cached,
                          output_cached);
    }

    printf("\nAND gate:\n");
    report_and_accuracy(&results, 0, 0);
    report_and_accuracy(&results, 0, 1);
    report_and_accuracy(&results, 1, 0);
    report_and_accuracy(&results, 1, 1);
    return_code = EXIT_SUCCESS;

cleanup:
    free_and_memory(&memory);
    return return_code;
}

int main(void)
{
    srand((unsigned int)time(NULL));
    init();

    if (test_fan2() != EXIT_SUCCESS || test_and_gate() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
