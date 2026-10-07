#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arch_primitives.h"
#include "got.h"

#define TRIALS 100000
#define WARMUP_TRIALS 1000
#define CACHE_LINE_BYTES 64
#define ADDRESS_PADDING_BYTES 128
#define ALLOCATION_BYTES 256

struct gate_memory {
    void *input1_allocation;
    void *input2_allocation;
    void *output_allocation;
    void *training_output_allocation;
    int *input1;
    int *input2;
    int *output;
    int *training_output;
};

struct accuracy_results {
    int totals[2][2];
    int correct[2][2];
};

static int predictor_training_input = 1;

static void run_nand(int *input1, int *input2, int *output, int *training_output)
{
    nand(&predictor_training_input, &predictor_training_input, training_output);
    nand(&predictor_training_input, &predictor_training_input, training_output);
    nand(&predictor_training_input, &predictor_training_input, training_output);
    nand(&predictor_training_input, &predictor_training_input, training_output);
    nand(input1, input2, output);
}

static int allocate_gate_memory(struct gate_memory *memory)
{
    memory->input1_allocation = aligned_alloc(CACHE_LINE_BYTES, ALLOCATION_BYTES);
    memory->input2_allocation = aligned_alloc(CACHE_LINE_BYTES, ALLOCATION_BYTES);
    memory->output_allocation = aligned_alloc(CACHE_LINE_BYTES, ALLOCATION_BYTES);
    memory->training_output_allocation = aligned_alloc(CACHE_LINE_BYTES, ALLOCATION_BYTES);

    if (memory->input1_allocation == NULL ||
         memory->input2_allocation == NULL ||
         memory->output_allocation == NULL ||
         memory->training_output_allocation == NULL) {
        return 0;
    }

    memset(memory->input1_allocation, 0, ALLOCATION_BYTES);
    memset(memory->input2_allocation, 0, ALLOCATION_BYTES);
    memset(memory->output_allocation, 0, ALLOCATION_BYTES);
    memset(memory->training_output_allocation, 0, ALLOCATION_BYTES);

    memory->input1 = (int *)((char *)memory->input1_allocation + ADDRESS_PADDING_BYTES);
    memory->input2 = (int *)((char *)memory->input2_allocation + ADDRESS_PADDING_BYTES);
    memory->output = (int *)((char *)memory->output_allocation + ADDRESS_PADDING_BYTES);
    memory->training_output = (int *)((char *)memory->training_output_allocation + ADDRESS_PADDING_BYTES);
    return 1;
}

static void free_gate_memory(struct gate_memory *memory)
{
    free(memory->input1_allocation);
    free(memory->input2_allocation);
    free(memory->output_allocation);
    free(memory->training_output_allocation);
}

static int run_trial(struct gate_memory *memory, int input1_cached,
                     int input2_cached)
{
    clear(memory->output);
    clear(memory->input1);
    clear(memory->input2);

    if (input1_cached) {
        set(memory->input1);
    }
    if (input2_cached) {
        set(memory->input2);
    }
    memory_fence();

    run_nand(memory->input1, memory->input2, memory->output, memory->training_output);
    return test(memory->output);
}

static void report_accuracy(const struct accuracy_results *results,
                            int input1_cached, int input2_cached)
{
    int expected_output = !(input1_cached && input2_cached);
    int total = results->totals[input1_cached][input2_cached];
    int correct = results->correct[input1_cached][input2_cached];

    printf("Inputs (%s, %s), expected output %s: %.2f%% accuracy "
           "(%d/%d correct)\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           expected_output ? "cached" : "uncached",
           100.0 * correct / total, correct, total);
}

int main(void)
{
    int return_code = EXIT_FAILURE;
    struct gate_memory memory = {0};
    struct accuracy_results results = {0};

    if (!allocate_gate_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    srand((unsigned int)time(NULL));
    init();

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        run_trial(&memory, rand() % 2, rand() % 2);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        int output_cached = run_trial(&memory, input1_cached, input2_cached);
        int expected_output = !(input1_cached && input2_cached);

        ++results.totals[input1_cached][input2_cached];
        results.correct[input1_cached][input2_cached] += (output_cached == expected_output);
    }

    report_accuracy(&results, 0, 0);
    report_accuracy(&results, 0, 1);
    report_accuracy(&results, 1, 0);
    report_accuracy(&results, 1, 1);

    return_code = EXIT_SUCCESS;

cleanup:
    free_gate_memory(&memory);
    return return_code;
}
