#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arch_primitives.h"
#include "got.h"

#define TRIALS 100000
#define WARMUP_TRIALS 1000
#define PAGE_BYTES 4096
#define ADDRESS_OFFSET_BYTES 128

struct cache_line {
    void *allocation;
    int *address;
};

struct gate_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line output1;
    struct cache_line output2;
    struct cache_line training_output1;
    struct cache_line training_output2;
};

struct accuracy_results {
    int totals[2][2];
    int output1_correct[2][2];
    int output2_correct[2][2];
    int both_correct[2][2];
    int outputs_equal[2][2];
};

struct observed_outputs {
    int output1_cached;
    int output2_cached;
};

static int predictor_training_input = 1;

static void run_nand2(int *input1, int *input2, int *output1, int *output2,
                      int *training_output1, int *training_output2)
{
    nand2(&predictor_training_input, &predictor_training_input, training_output1, training_output2);
    nand2(&predictor_training_input, &predictor_training_input, training_output1, training_output2);
    nand2(&predictor_training_input, &predictor_training_input, training_output1, training_output2);
    nand2(&predictor_training_input, &predictor_training_input, training_output1, training_output2);
    nand2(input1, input2, output1, output2);
}

static int allocate_cache_line(struct cache_line *line)
{
    line->allocation = aligned_alloc(PAGE_BYTES, PAGE_BYTES);
    if (line->allocation == NULL) {
        return 0;
    }

    memset(line->allocation, 0, PAGE_BYTES);
    line->address = (int *)((char *)line->allocation + ADDRESS_OFFSET_BYTES);
    return 1;
}

static int allocate_gate_memory(struct gate_memory *memory)
{
    if (!allocate_cache_line(&memory->input1)
        || !allocate_cache_line(&memory->input2)
        || !allocate_cache_line(&memory->output1)
        || !allocate_cache_line(&memory->output2)
        || !allocate_cache_line(&memory->training_output1)
        || !allocate_cache_line(&memory->training_output2)) {
        return 0;
    }

    return 1;
}

static void free_cache_line(struct cache_line *line)
{
    free(line->allocation);
}

static void free_gate_memory(struct gate_memory *memory)
{
    free_cache_line(&memory->input1);
    free_cache_line(&memory->input2);
    free_cache_line(&memory->output1);
    free_cache_line(&memory->output2);
    free_cache_line(&memory->training_output1);
    free_cache_line(&memory->training_output2);
}

static struct observed_outputs run_trial(struct gate_memory *memory,
                                         int input1_cached,
                                         int input2_cached)
{
    struct observed_outputs outputs;

    clear(memory->output1.address);
    clear(memory->output2.address);
    clear(memory->input1.address);
    clear(memory->input2.address);

    if (input1_cached) {
        set(memory->input1.address);
    }
    if (input2_cached) {
        set(memory->input2.address);
    }
    memory_fence();

    run_nand2(memory->input1.address, memory->input2.address,
              memory->output1.address, memory->output2.address,
              memory->training_output1.address,
              memory->training_output2.address);
    outputs.output1_cached = test(memory->output1.address);
    outputs.output2_cached = test(memory->output2.address);
    return outputs;
}

static void record_result(struct accuracy_results *results,
                          int input1_cached, int input2_cached,
                          struct observed_outputs outputs)
{
    int expected_output = !(input1_cached && input2_cached);
    int output1_correct = outputs.output1_cached == expected_output;
    int output2_correct = outputs.output2_cached == expected_output;

    ++results->totals[input1_cached][input2_cached];
    results->output1_correct[input1_cached][input2_cached] += output1_correct;
    results->output2_correct[input1_cached][input2_cached] += output2_correct;
    results->both_correct[input1_cached][input2_cached] += output1_correct && output2_correct;
    results->outputs_equal[input1_cached][input2_cached] +=
        outputs.output1_cached == outputs.output2_cached;
}

static void print_rate(const char *label, int correct, int total)
{
    printf("  %s: %.2f%% (%d/%d)\n",
           label, 100.0 * correct / total, correct, total);
}

static void report_accuracy(const struct accuracy_results *results,
                            int input1_cached, int input2_cached)
{
    int expected_output = !(input1_cached && input2_cached);
    int total = results->totals[input1_cached][input2_cached];

    printf("Inputs (%s, %s), expected outputs %s:\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           expected_output ? "cached" : "uncached");
    print_rate("Output 1 correct",
               results->output1_correct[input1_cached][input2_cached], total);
    print_rate("Output 2 correct",
               results->output2_correct[input1_cached][input2_cached], total);
    print_rate("Both correct",
               results->both_correct[input1_cached][input2_cached], total);
    print_rate("Outputs identical",
               results->outputs_equal[input1_cached][input2_cached], total);
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
        struct observed_outputs outputs =
            run_trial(&memory, input1_cached, input2_cached);

        record_result(&results, input1_cached, input2_cached, outputs);
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
