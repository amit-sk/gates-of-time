#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arch_primitives.h"
#include "cache_level.h"

#define CACHE_LEVEL_COUNT 4
#define TARGET_COUNT 64
#define TRIALS_PER_LEVEL 2500

struct cache_topology {
    int cpu;
    size_t page_bytes;
    size_t line_bytes;
    size_t l1_bytes;
    size_t l2_bytes;
    size_t llc_bytes;
};

struct pointer_cycle {
    unsigned char *allocation;
    void *cursor;
    size_t bytes;
    size_t lines;
};

struct target_line {
    int *address;
    volatile int *translation_helper;
};

struct target_pool {
    unsigned char *allocation;
    struct target_line lines[TARGET_COUNT];
};

struct test_results {
    size_t confusion[CACHE_LEVEL_COUNT][CACHE_LEVEL_COUNT];
};

static uint64_t random_state = UINT64_C(0xc4f32663d8b74e91);
static void *volatile traversal_sink;

static uint64_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static void shuffle_levels(int levels[CACHE_LEVEL_COUNT])
{
    for (size_t i = CACHE_LEVEL_COUNT; i > 1; --i) {
        size_t other = (size_t)(next_random() % i);
        int temporary = levels[i - 1];
        levels[i - 1] = levels[other];
        levels[other] = temporary;
    }
}

static void shuffle_indices(size_t *indices, size_t count)
{
    for (size_t i = count; i > 1; --i) {
        size_t other = (size_t)(next_random() % i);
        size_t temporary = indices[i - 1];
        indices[i - 1] = indices[other];
        indices[other] = temporary;
    }
}

static int pin_to_current_cpu(void)
{
    int cpu = sched_getcpu();
    cpu_set_t cpu_set;

    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        if (cpu >= CPU_SETSIZE) {
            errno = EINVAL;
        }
        return 0;
    }

    CPU_ZERO(&cpu_set);
    CPU_SET(cpu, &cpu_set);
    return sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == 0;
}

static int detect_cache_topology(struct cache_topology *topology)
{
    long page_bytes = sysconf(_SC_PAGESIZE);
    long line_bytes = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    long l1_bytes = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    long l2_bytes = sysconf(_SC_LEVEL2_CACHE_SIZE);
    long llc_bytes = sysconf(_SC_LEVEL3_CACHE_SIZE);

    topology->cpu = sched_getcpu();
    if (topology->cpu < 0 || page_bytes <= 0 || line_bytes <= 0
        || l1_bytes <= 0 || l2_bytes <= 0 || llc_bytes <= 0) {
        return 0;
    }

    topology->page_bytes = (size_t)page_bytes;
    topology->line_bytes = (size_t)line_bytes;
    topology->l1_bytes = (size_t)l1_bytes;
    topology->l2_bytes = (size_t)l2_bytes;
    topology->llc_bytes = (size_t)llc_bytes;
    return topology->page_bytes % topology->line_bytes == 0;
}

static int warm_up_cpu(void)
{
    struct timespec start;
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 0;
    }

    do {
        for (int i = 0; i < 100000; ++i) {
            asm volatile("" ::: "memory");
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return 0;
        }
    } while (now.tv_sec == start.tv_sec
             || (now.tv_sec == start.tv_sec + 1
                 && now.tv_nsec < start.tv_nsec));
    return 1;
}

static size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static int initialize_pointer_cycle(struct pointer_cycle *cycle,
                                    size_t requested_bytes,
                                    const struct cache_topology *topology)
{
    cycle->bytes = round_up(requested_bytes, topology->line_bytes);
    cycle->lines = cycle->bytes / topology->line_bytes;
    if (cycle->lines < 2) {
        return 0;
    }

    if (posix_memalign((void **)&cycle->allocation, topology->page_bytes,
                       cycle->bytes) != 0) {
        cycle->allocation = NULL;
        return 0;
    }
    memset(cycle->allocation, 0, cycle->bytes);

    size_t *order = calloc(cycle->lines, sizeof(*order));
    if (order == NULL) {
        free(cycle->allocation);
        cycle->allocation = NULL;
        return 0;
    }

    for (size_t i = 0; i < cycle->lines; ++i) {
        order[i] = i;
    }
    shuffle_indices(order, cycle->lines);

    for (size_t i = 0; i < cycle->lines; ++i) {
        size_t next = (i + 1) % cycle->lines;
        void **node = (void **)(cycle->allocation
                                + order[i] * topology->line_bytes);
        *node = cycle->allocation + order[next] * topology->line_bytes;
    }
    cycle->cursor = cycle->allocation + order[0] * topology->line_bytes;
    free(order);
    return 1;
}

static void free_pointer_cycle(struct pointer_cycle *cycle)
{
    free(cycle->allocation);
    cycle->allocation = NULL;
    cycle->cursor = NULL;
    cycle->bytes = 0;
    cycle->lines = 0;
}

static void traverse_cycle(struct pointer_cycle *cycle)
{
    void *cursor = cycle->cursor;

    for (size_t i = 0; i < cycle->lines; ++i) {
        cursor = *(void *volatile *)cursor;
    }
    cycle->cursor = cursor;
    traversal_sink = cursor;
}

static int initialize_target_pool(struct target_pool *pool,
                                  const struct cache_topology *topology)
{
    size_t allocation_bytes = TARGET_COUNT * topology->page_bytes;
    size_t lines_per_page = topology->page_bytes / topology->line_bytes;

    if (lines_per_page < 4
        || posix_memalign((void **)&pool->allocation, topology->page_bytes,
                          allocation_bytes) != 0) {
        pool->allocation = NULL;
        return 0;
    }
    memset(pool->allocation, 0, allocation_bytes);

    for (size_t i = 0; i < TARGET_COUNT; ++i) {
        unsigned char *page = pool->allocation + i * topology->page_bytes;
        size_t target_line = i % lines_per_page;
        size_t helper_line =
            (target_line + lines_per_page / 2) % lines_per_page;
        pool->lines[i].address =
            (int *)(page + target_line * topology->line_bytes);
        pool->lines[i].translation_helper =
            (volatile int *)(page + helper_line * topology->line_bytes);
    }
    return 1;
}

static void free_target_pool(struct target_pool *pool)
{
    free(pool->allocation);
    pool->allocation = NULL;
}

static void warm_target(int *target)
{
    (void)*(volatile int *)target;
    memory_fence();
}

static void prepare_level(int level,
                          struct target_line *target,
                          struct pointer_cycle *l1_eviction,
                          struct pointer_cycle *l2_eviction)
{
    if (level == 1) {
        (void)*(volatile int *)target->address;
    } else if (level == 2) {
        warm_target(target->address);
        traverse_cycle(l1_eviction);
    } else if (level == 3) {
        warm_target(target->address);
        traverse_cycle(l2_eviction);
    } else {
        memory_flush(target->address);
        memory_fence();
    }

    (void)*target->translation_helper;
}

static size_t row_total(const struct test_results *results, int expected)
{
    size_t total = 0;

    for (int observed = 0; observed < CACHE_LEVEL_COUNT; ++observed) {
        total += results->confusion[expected][observed];
    }
    return total;
}

static void print_results(const struct test_results *results)
{
    static const char *names[CACHE_LEVEL_COUNT] = {
        "uncached", "L1", "L2", "LLC",
    };
    size_t total = 0;
    size_t correct = 0;

    printf("Prepared-state agreement (rows expected, columns returned):\n");
    printf("%-10s %10s %10s %10s %10s %12s\n",
           "expected", "uncached", "L1", "L2", "LLC", "agreement");

    for (int expected = 0; expected < CACHE_LEVEL_COUNT; ++expected) {
        size_t expected_total = row_total(results, expected);
        size_t expected_correct = results->confusion[expected][expected];

        printf("%-10s", names[expected]);
        for (int observed = 0; observed < CACHE_LEVEL_COUNT; ++observed) {
            printf(" %10zu", results->confusion[expected][observed]);
        }
        printf(" %10.2f%%\n",
               100.0 * (double)expected_correct / (double)expected_total);

        total += expected_total;
        correct += expected_correct;
    }

    printf("Overall: %.2f%% agreement (%zu/%zu)\n",
           100.0 * (double)correct / (double)total, correct, total);
}

int main(void)
{
    struct cache_topology topology = {0};
    struct pointer_cycle l1_eviction = {0};
    struct pointer_cycle l2_eviction = {0};
    struct target_pool targets = {0};
    struct test_results results = {0};
    int return_code = EXIT_FAILURE;

    if (!pin_to_current_cpu()) {
        perror("sched_setaffinity");
        goto cleanup;
    }
    if (!detect_cache_topology(&topology)) {
        fprintf(stderr, "could not detect cache topology for the current CPU\n");
        goto cleanup;
    }
    if (!initialize_target_pool(&targets, &topology)
        || !initialize_pointer_cycle(&l1_eviction, topology.l1_bytes * 4,
                                     &topology)
        || !initialize_pointer_cycle(&l2_eviction, topology.l2_bytes * 4,
                                     &topology)) {
        fprintf(stderr, "could not allocate test buffers\n");
        goto cleanup;
    }

    if (!warm_up_cpu()) {
        perror("clock_gettime");
        goto cleanup;
    }
    traverse_cycle(&l1_eviction);
    traverse_cycle(&l2_eviction);

    printf("CPU %d: L1d=%zu KiB, L2=%zu KiB, LLC=%zu KiB\n",
           topology.cpu,
           topology.l1_bytes / 1024,
           topology.l2_bytes / 1024,
           topology.llc_bytes / 1024);

    for (size_t trial = 0; trial < TRIALS_PER_LEVEL; ++trial) {
        int levels[CACHE_LEVEL_COUNT] = {0, 1, 2, 3};
        shuffle_levels(levels);

        for (int position = 0; position < CACHE_LEVEL_COUNT; ++position) {
            int expected = levels[position];
            size_t target_index = (size_t)(next_random() % TARGET_COUNT);
            struct target_line *target = &targets.lines[target_index];

            prepare_level(expected, target, &l1_eviction, &l2_eviction);
            int observed = cacheLevel(target->address);
            if (observed < 0 || observed >= CACHE_LEVEL_COUNT) {
                fprintf(stderr, "cacheLevel returned invalid level %d\n",
                        observed);
                goto cleanup;
            }
            ++results.confusion[expected][observed];
        }
    }

    print_results(&results);
    return_code = EXIT_SUCCESS;

cleanup:
    free_target_pool(&targets);
    free_pointer_cycle(&l1_eviction);
    free_pointer_cycle(&l2_eviction);
    return return_code;
}
