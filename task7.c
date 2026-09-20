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
#define BIT_COUNT 3
#define VALUE_COUNT (1 << BIT_COUNT)

struct cache_line {
    void *allocation;
    int *address;
};

struct adder_outputs {
    int sum_cached;
    int carry_cached;
};

struct full_adder_memory {
    struct cache_line input1;
    struct cache_line input2;
    struct cache_line carry_in;
    struct cache_line sum;
    struct cache_line carry_out;
};

struct full_adder_accuracy_results {
    int totals[2][2][2];
    int sum_correct[2][2][2];
    int carry_correct[2][2][2];
    int both_correct[2][2][2];
};

struct adder3_memory {
    struct cache_line input1[BIT_COUNT];
    struct cache_line input2[BIT_COUNT];
    struct cache_line sum[BIT_COUNT];
};

struct adder3_accuracy_results {
    int total;
    int exact_sum_correct;
    int bit_correct[BIT_COUNT];
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

static void free_cache_line(struct cache_line *line)
{
    free(line->allocation);
}

static void print_rate(const char *label, int correct, int total)
{
    printf("  %s: %.2f%% (%d/%d)\n",
           label, 100.0 * correct / total, correct, total);
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
    struct adder_outputs *outputs)
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
    const struct adder_outputs *outputs)
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
        struct adder_outputs outputs;
        trash = run_full_adder_trial(&memory, rand() % 2, rand() % 2,
                                     rand() % 2, trash, &outputs);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1_cached = rand() % 2;
        int input2_cached = rand() % 2;
        int carry_in_cached = rand() % 2;
        struct adder_outputs outputs;

        trash = run_full_adder_trial(&memory, input1_cached, input2_cached,
                                     carry_in_cached, trash, &outputs);
        record_full_adder_result(&results, input1_cached, input2_cached,
                                 carry_in_cached, &outputs);
    }

    printf("Full adder:\n");
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

static int allocate_adder3_memory(struct adder3_memory *memory)
{
    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        if (!allocate_cache_line(&memory->input1[bit])
            || !allocate_cache_line(&memory->input2[bit])
            || !allocate_cache_line(&memory->sum[bit])) {
            return 0;
        }
    }

    return 1;
}

static void free_adder3_memory(struct adder3_memory *memory)
{
    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        free_cache_line(&memory->input1[bit]);
        free_cache_line(&memory->input2[bit]);
        free_cache_line(&memory->sum[bit]);
    }
}

static uintptr_t run_adder3_trial(struct adder3_memory *memory,
                                  int input1,
                                  int input2,
                                  uintptr_t trash,
                                  int *observed_sum)
{
    uintptr_t input1_addresses[BIT_COUNT];
    uintptr_t input2_addresses[BIT_COUNT];
    uintptr_t sum_addresses[BIT_COUNT];

    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        clear(memory->input1[bit].address);
        clear(memory->input2[bit].address);
        clear(memory->sum[bit].address);

        if (input1 & (1 << bit)) {
            set(memory->input1[bit].address);
        }
        if (input2 & (1 << bit)) {
            set(memory->input2[bit].address);
        }

        input1_addresses[bit] = (uintptr_t)memory->input1[bit].address;
        input2_addresses[bit] = (uintptr_t)memory->input2[bit].address;
        sum_addresses[bit] = (uintptr_t)memory->sum[bit].address;
    }
    memory_fence();

    trash = adder3_impl(input1_addresses, input2_addresses,
                        sum_addresses, trash);

    *observed_sum = 0;
    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        *observed_sum |= test(memory->sum[bit].address) << bit;
    }
    return trash;
}

static void record_adder3_result(struct adder3_accuracy_results *results,
                                 int expected_sum,
                                 int observed_sum)
{
    ++results->total;
    results->exact_sum_correct += observed_sum == expected_sum;

    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        int expected_bit = (expected_sum >> bit) & 1;
        int observed_bit = (observed_sum >> bit) & 1;
        results->bit_correct[bit] += observed_bit == expected_bit;
    }
}

static int test_adder3(void)
{
    int return_code = EXIT_FAILURE;
    struct adder3_memory memory = {0};
    struct adder3_accuracy_results results = {0};
    uintptr_t trash = 0;

    if (!allocate_adder3_memory(&memory)) {
        perror("aligned_alloc");
        goto cleanup;
    }

    for (int trial = 0; trial < WARMUP_TRIALS; ++trial) {
        int input1 = rand() % VALUE_COUNT;
        int input2 = rand() % VALUE_COUNT;
        int observed_sum;

        trash = run_adder3_trial(&memory, input1, input2,
                                 trash, &observed_sum);
    }

    for (int trial = 0; trial < TRIALS; ++trial) {
        int input1 = rand() % VALUE_COUNT;
        int input2 = rand() % VALUE_COUNT;
        int expected_sum = (input1 + input2) % VALUE_COUNT;
        int observed_sum;

        trash = run_adder3_trial(&memory, input1, input2,
                                 trash, &observed_sum);
        record_adder3_result(&results, expected_sum, observed_sum);
    }

    printf("\nThree-bit adder:\n");
    print_rate("Exact sum correct", results.exact_sum_correct, results.total);
    for (int bit = 0; bit < BIT_COUNT; ++bit) {
        char label[32];
        snprintf(label, sizeof(label), "Sum bit %d correct", bit);
        print_rate(label, results.bit_correct[bit], results.total);
    }
    return_code = EXIT_SUCCESS;

cleanup:
    free_adder3_memory(&memory);
    return return_code;
}

int main(void)
{
    srand((unsigned int)time(NULL));
    init();

    if (test_full_adder() != EXIT_SUCCESS
        || test_adder3() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
