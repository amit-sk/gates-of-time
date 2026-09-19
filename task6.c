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

typedef uintptr_t (*binary_gate)(uintptr_t input1, uintptr_t input2,
                                 uintptr_t output, uintptr_t trash);

typedef int (*binary_operation)(int input1, int input2);

struct binary_gate_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line output;
};

struct binary_gate_accuracy_results {
    int totals[2][2];
    int correct[2][2];
};

struct half_adder_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line sum;
    struct cache_line carry;
};

struct half_adder_outputs {
    int sum_cached;
    int carry_cached;
};

struct half_adder_accuracy_results {
    int totals[2][2];
    int sum_correct[2][2];
    int carry_correct[2][2];
    int both_correct[2][2];
};

struct full_adder_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line carry_in;
    struct cache_line sum;
    struct cache_line carry_out;
};

struct full_adder_outputs {
    int sum_cached;
    int carry_cached;
};

struct full_adder_accuracy_results {
    int totals[2][2][2];
    int sum_correct[2][2][2];
    int carry_correct[2][2][2];
    int both_correct[2][2][2];
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

static int allocate_binary_gate_memory(struct binary_gate_memory *memory)
{
    if (!allocate_cache_line(&memory->input1)
        || !allocate_cache_line(&memory->input2)
        || !allocate_cache_line(&memory->output)) {
        return 0;
    }

    return 1;
}

static void free_binary_gate_memory(struct binary_gate_memory *memory)
{
    free_cache_line(&memory->input1);
    free_cache_line(&memory->input2);
    free_cache_line(&memory->output);
}

static uintptr_t run_binary_gate_trial(struct binary_gate_memory *memory,
                                       binary_gate gate,
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

    trash = gate((uintptr_t)memory->input1.address,
                 (uintptr_t)memory->input2.address,
                 (uintptr_t)memory->output.address, trash);
    *output_cached = test(memory->output.address);
    return trash;
}

static void record_binary_gate_result(
    struct binary_gate_accuracy_results *results,
    binary_operation operation,
    int input1_cached,
    int input2_cached,
    int output_cached)
{
    int expected_output = operation(input1_cached, input2_cached);

    ++results->totals[input1_cached][input2_cached];
    results->correct[input1_cached][input2_cached] +=
        output_cached == expected_output;
}

static void report_binary_gate_accuracy(
    const struct binary_gate_accuracy_results *results,
    binary_operation operation,
    int input1_cached,
    int input2_cached)
{
    int total = results->totals[input1_cached][input2_cached];
    int correct = results->correct[input1_cached][input2_cached];
    int expected_output = operation(input1_cached, input2_cached);

    printf("Inputs (%s, %s), expected output %s: %.2f%% (%d/%d)\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           expected_output ? "cached" : "uncached",
           100.0 * correct / total, correct, total);
}

static int test_binary_gate(const char *name, binary_gate gate,
                            binary_operation operation)
{
    int return_code = EXIT_FAILURE;
    struct binary_gate_memory memory = {0};
    struct binary_gate_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_binary_gate_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        int output_cached;
        trash = run_binary_gate_trial(&memory, gate, rand() % 2, rand() % 2,
                                      trash, &output_cached);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        int output_cached;

        trash = run_binary_gate_trial(&memory, gate, input1_cached,
                                      input2_cached, trash, &output_cached);
        record_binary_gate_result(&results, operation, input1_cached,
                                  input2_cached, output_cached);
    }

    printf("\n%s:\n", name);
    report_binary_gate_accuracy(&results, operation, 0, 0);
    report_binary_gate_accuracy(&results, operation, 0, 1);
    report_binary_gate_accuracy(&results, operation, 1, 0);
    report_binary_gate_accuracy(&results, operation, 1, 1);
    return_code = EXIT_SUCCESS;

cleanup:
    free_binary_gate_memory(&memory);
    return return_code;
}

static int and_operation(int input1, int input2)
{
    return input1 && input2;
}

static int or_operation(int input1, int input2)
{
    return input1 || input2;
}

static int allocate_half_adder_memory(struct half_adder_memory *memory)
{
    if (!allocate_cache_line(&memory->input1)
        || !allocate_cache_line(&memory->input2)
        || !allocate_cache_line(&memory->sum)
        || !allocate_cache_line(&memory->carry)) {
        return 0;
    }

    return 1;
}

static void free_half_adder_memory(struct half_adder_memory *memory)
{
    free_cache_line(&memory->input1);
    free_cache_line(&memory->input2);
    free_cache_line(&memory->sum);
    free_cache_line(&memory->carry);
}

static uintptr_t run_half_adder_trial(
    struct half_adder_memory *memory,
    int input1_cached,
    int input2_cached,
    uintptr_t trash,
    struct half_adder_outputs *outputs)
{
    clear(memory->input1.address);
    clear(memory->input2.address);
    clear(memory->sum.address);
    clear(memory->carry.address);

    if (input1_cached) {
        set(memory->input1.address);
    }
    if (input2_cached) {
        set(memory->input2.address);
    }
    memory_fence();

    trash = half_adder_impl((uintptr_t)memory->input1.address,
                            (uintptr_t)memory->input2.address,
                            (uintptr_t)memory->sum.address,
                            (uintptr_t)memory->carry.address, trash);
    outputs->sum_cached = test(memory->sum.address);
    outputs->carry_cached = test(memory->carry.address);
    return trash;
}

static void record_half_adder_result(
    struct half_adder_accuracy_results *results,
    int input1_cached,
    int input2_cached,
    const struct half_adder_outputs *outputs)
{
    int expected_sum = input1_cached ^ input2_cached;
    int expected_carry = input1_cached && input2_cached;
    int sum_correct = outputs->sum_cached == expected_sum;
    int carry_correct = outputs->carry_cached == expected_carry;

    ++results->totals[input1_cached][input2_cached];
    results->sum_correct[input1_cached][input2_cached] += sum_correct;
    results->carry_correct[input1_cached][input2_cached] += carry_correct;
    results->both_correct[input1_cached][input2_cached] +=
        sum_correct && carry_correct;
}

static void report_half_adder_accuracy(
    const struct half_adder_accuracy_results *results,
    int input1_cached,
    int input2_cached)
{
    int expected_sum = input1_cached ^ input2_cached;
    int expected_carry = input1_cached && input2_cached;
    int total = results->totals[input1_cached][input2_cached];

    printf("Inputs (%s, %s), expected sum %s, carry %s:\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           expected_sum ? "cached" : "uncached",
           expected_carry ? "cached" : "uncached");
    print_rate("Sum correct",
               results->sum_correct[input1_cached][input2_cached], total);
    print_rate("Carry correct",
               results->carry_correct[input1_cached][input2_cached], total);
    print_rate("Both correct",
               results->both_correct[input1_cached][input2_cached], total);
}

static int test_half_adder(void)
{
    int return_code = EXIT_FAILURE;
    struct half_adder_memory memory = {0};
    struct half_adder_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_half_adder_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        struct half_adder_outputs outputs;
        trash = run_half_adder_trial(&memory, rand() % 2, rand() % 2,
                                     trash, &outputs);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        struct half_adder_outputs outputs;

        trash = run_half_adder_trial(&memory, input1_cached, input2_cached,
                                     trash, &outputs);
        record_half_adder_result(&results, input1_cached, input2_cached,
                                 &outputs);
    }

    printf("\nHalf adder:\n");
    report_half_adder_accuracy(&results, 0, 0);
    report_half_adder_accuracy(&results, 0, 1);
    report_half_adder_accuracy(&results, 1, 0);
    report_half_adder_accuracy(&results, 1, 1);
    return_code = EXIT_SUCCESS;

cleanup:
    free_half_adder_memory(&memory);
    return return_code;
}

static int allocate_full_adder_memory(struct full_adder_memory *memory)
{
    if (!allocate_cache_line(&memory->input1)
        || !allocate_cache_line(&memory->input2)
        || !allocate_cache_line(&memory->carry_in)
        || !allocate_cache_line(&memory->sum)
        || !allocate_cache_line(&memory->carry_out)) {
        return 0;
    }

    return 1;
}

static void free_full_adder_memory(struct full_adder_memory *memory)
{
    free_cache_line(&memory->input1);
    free_cache_line(&memory->input2);
    free_cache_line(&memory->carry_in);
    free_cache_line(&memory->sum);
    free_cache_line(&memory->carry_out);
}

static uintptr_t run_full_adder_trial(
    struct full_adder_memory *memory,
    int input1_cached,
    int input2_cached,
    int carry_in_cached,
    uintptr_t trash,
    struct full_adder_outputs *outputs)
{
    clear(memory->input1.address);
    clear(memory->input2.address);
    clear(memory->carry_in.address);
    clear(memory->sum.address);
    clear(memory->carry_out.address);

    if (input1_cached) {
        set(memory->input1.address);
    }
    if (input2_cached) {
        set(memory->input2.address);
    }
    if (carry_in_cached) {
        set(memory->carry_in.address);
    }
    memory_fence();

    trash = full_adder_impl((uintptr_t)memory->input1.address,
                            (uintptr_t)memory->input2.address,
                            (uintptr_t)memory->carry_in.address,
                            (uintptr_t)memory->sum.address,
                            (uintptr_t)memory->carry_out.address, trash);
    outputs->sum_cached = test(memory->sum.address);
    outputs->carry_cached = test(memory->carry_out.address);
    return trash;
}

static void record_full_adder_result(
    struct full_adder_accuracy_results *results,
    int input1_cached,
    int input2_cached,
    int carry_in_cached,
    const struct full_adder_outputs *outputs)
{
    int input_sum = input1_cached + input2_cached + carry_in_cached;
    int expected_sum = input_sum % 2;
    int expected_carry = input_sum >= 2;
    int sum_correct = outputs->sum_cached == expected_sum;
    int carry_correct = outputs->carry_cached == expected_carry;

    ++results->totals[input1_cached][input2_cached][carry_in_cached];
    results->sum_correct[input1_cached][input2_cached][carry_in_cached] +=
        sum_correct;
    results->carry_correct[input1_cached][input2_cached][carry_in_cached] +=
        carry_correct;
    results->both_correct[input1_cached][input2_cached][carry_in_cached] +=
        sum_correct && carry_correct;
}

static void report_full_adder_accuracy(
    const struct full_adder_accuracy_results *results,
    int input1_cached,
    int input2_cached,
    int carry_in_cached)
{
    int input_sum = input1_cached + input2_cached + carry_in_cached;
    int expected_sum = input_sum % 2;
    int expected_carry = input_sum >= 2;
    int total = results->totals[input1_cached][input2_cached][carry_in_cached];

    printf("Inputs (%s, %s), carry-in %s, expected sum %s, carry %s:\n",
           input1_cached ? "cached" : "uncached",
           input2_cached ? "cached" : "uncached",
           carry_in_cached ? "cached" : "uncached",
           expected_sum ? "cached" : "uncached",
           expected_carry ? "cached" : "uncached");
    print_rate(
        "Sum correct",
        results->sum_correct[input1_cached][input2_cached][carry_in_cached],
        total);
    print_rate(
        "Carry correct",
        results->carry_correct[input1_cached][input2_cached][carry_in_cached],
        total);
    print_rate(
        "Both correct",
        results->both_correct[input1_cached][input2_cached][carry_in_cached],
        total);
}

static int test_full_adder(void)
{
    int return_code = EXIT_FAILURE;
    struct full_adder_memory memory = {0};
    struct full_adder_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_full_adder_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        struct full_adder_outputs outputs;
        trash = run_full_adder_trial(&memory, rand() % 2, rand() % 2,
                                     rand() % 2, trash, &outputs);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        int carry_in_cached = rand() % 2;
        struct full_adder_outputs outputs;

        trash = run_full_adder_trial(&memory, input1_cached, input2_cached,
                                     carry_in_cached, trash, &outputs);
        record_full_adder_result(&results, input1_cached, input2_cached,
                                 carry_in_cached, &outputs);
    }

    printf("\nFull adder:\n");
    for (int input1_cached = 0; input1_cached < 2; ++input1_cached) {
        for (int input2_cached = 0; input2_cached < 2; ++input2_cached) {
            for (int carry_in_cached = 0; carry_in_cached < 2;
                 ++carry_in_cached) {
                report_full_adder_accuracy(&results, input1_cached,
                                           input2_cached, carry_in_cached);
            }
        }
    }
    return_code = EXIT_SUCCESS;

cleanup:
    free_full_adder_memory(&memory);
    return return_code;
}

int main(void)
{
    srand((unsigned int)time(NULL));
    init();

    if (test_fan2() != EXIT_SUCCESS
        || test_binary_gate("AND gate", and, and_operation) != EXIT_SUCCESS
        || test_binary_gate("OR gate", or, or_operation) != EXIT_SUCCESS
        || test_half_adder() != EXIT_SUCCESS
        || test_full_adder() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
