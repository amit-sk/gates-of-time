#include <stdint.h>
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

typedef uintptr_t (*not_function)(int *input, int *output,
                                  int *training_output, uintptr_t trash);

struct not_implementation {
    const char *name;
    not_function run;
};

struct gate_memory {
    void *input_allocation;
    void *output_allocation;
    void *training_output_allocation;
    int *input;
    int *output;
    int *training_output;
};

struct accuracy_results {
    int input_totals[2];
    int correct_outputs[2];
};

static int predictor_training_input = 1;

static uintptr_t run_not(int *input, int *output, int *training_output,
                         uintptr_t trash)
{
    not(&predictor_training_input, training_output);
    not(&predictor_training_input, training_output);
    not(&predictor_training_input, training_output);
    not(&predictor_training_input, training_output);
    not(input, output);
    return trash;
}

static uintptr_t run_imul_not(int *input, int *output, int *training_output,
                              uintptr_t trash)
{
    imul_not(&predictor_training_input, training_output);
    imul_not(&predictor_training_input, training_output);
    imul_not(&predictor_training_input, training_output);
    imul_not(&predictor_training_input, training_output);
    imul_not(input, output);
    return trash;
}

static uintptr_t run_imul_not2(int *input, int *output, int *training_output,
                               uintptr_t trash)
{
    (void)training_output;
    return imul_not2((uintptr_t)input, (uintptr_t)output, trash);
}

static int allocate_gate_memory(struct gate_memory *memory)
{
    memory->input_allocation = aligned_alloc(CACHE_LINE_BYTES,
                                             ALLOCATION_BYTES);
    memory->output_allocation = aligned_alloc(CACHE_LINE_BYTES,
                                              ALLOCATION_BYTES);
    memory->training_output_allocation = aligned_alloc(CACHE_LINE_BYTES,
                                                       ALLOCATION_BYTES);

    if (memory->input_allocation == NULL || memory->output_allocation == NULL
        || memory->training_output_allocation == NULL)
        return 0;

    memset(memory->input_allocation, 0, ALLOCATION_BYTES);
    memset(memory->output_allocation, 0, ALLOCATION_BYTES);
    memset(memory->training_output_allocation, 0, ALLOCATION_BYTES);
    memory->input = (int *)((char *)memory->input_allocation
                           + ADDRESS_PADDING_BYTES);
    memory->output = (int *)((char *)memory->output_allocation
                            + ADDRESS_PADDING_BYTES);
    memory->training_output = (int *)((char *)memory->training_output_allocation
                                     + ADDRESS_PADDING_BYTES);
    return 1;
}

static void free_gate_memory(struct gate_memory *memory)
{
    free(memory->input_allocation);
    free(memory->output_allocation);
    free(memory->training_output_allocation);
}

static uintptr_t run_trial(const struct not_implementation *implementation,
                           struct gate_memory *memory, int input_cached,
                           uintptr_t trash, int *output_cached)
{
    clear(memory->output);
    clear(memory->input);
    if (input_cached)
        set(memory->input);
    memory_fence();

    trash = implementation->run(memory->input, memory->output,
                                memory->training_output,
                                trash);
    *output_cached = test(memory->output);
    return trash;
}

static void report_accuracy(const char *input_state, int correct, int total)
{
    printf("  %s input: %.2f%% accuracy (%d/%d correct)\n",
           input_state, 100.0 * correct / total, correct, total);
}

static int test_implementation(const struct not_implementation *implementation,
                               const unsigned char input_states[TRIALS])
{
    struct gate_memory memory = {0};
    struct accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_gate_memory(&memory)) {
        perror("aligned_alloc");
        free_gate_memory(&memory);
        return 0;
    }

    init();

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        int output_cached;
        trash = run_trial(implementation, &memory, input_states[trial], trash,
                          &output_cached);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input_cached = input_states[trial];
        int output_cached;

        trash = run_trial(implementation, &memory, input_cached, trash,
                          &output_cached);
        ++results.input_totals[input_cached];
        results.correct_outputs[input_cached] += output_cached == !input_cached;
    }

    printf("%s:\n", implementation->name);
    report_accuracy("Cached", results.correct_outputs[1],
                    results.input_totals[1]);
    report_accuracy("Uncached", results.correct_outputs[0],
                    results.input_totals[0]);

    free_gate_memory(&memory);
    return 1;
}

int main(void)
{
    static const struct not_implementation implementations[] = {
        {"not", run_not},
        {"imul_not", run_imul_not},
        {"imul_not2", run_imul_not2},
    };
    unsigned char input_states[TRIALS];

    srand((unsigned int)time(NULL));
    for (int trial = 0; trial < TRIALS; ++trial)
        input_states[trial] = rand() % 2;

    for (size_t i = 0; i < sizeof(implementations) / sizeof(implementations[0]);
         ++i) {
        if (!test_implementation(&implementations[i], input_states))
            return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
