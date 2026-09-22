#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arch_primitives.h"
#include "cache_level.h"

#define DEFAULT_SAMPLES 5000
#define MAX_CACHE_INDEX 16
#define TARGET_PAGE_BYTES 4096
#define TARGET_OFFSET_BYTES 128
#define TRANSLATION_OFFSET_BYTES 3072
#define PREPARED_STATE_COUNT 4
#define WORKING_SET_COUNT 4

struct cache_topology {
    int cpu;
    size_t line_bytes;
    size_t l1_bytes;
    size_t l2_bytes;
    size_t llc_bytes;
};

struct pointer_cycle {
    unsigned char *allocation;
    uintptr_t cursor;
    size_t bytes;
    size_t lines;
};

struct timing_record {
    const char *mode;
    const char *label;
    int expected_level;
    size_t working_set_bytes;
    size_t sample;
    uint64_t latency_ticks;
};

struct prepared_state {
    const char *label;
    int expected_level;
};

static uint64_t random_state = UINT64_C(0x8f3f73b5cf1c9ad7);
static volatile uintptr_t traversal_sink;

static uint64_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
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

static int read_text_file(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }

    int success = fgets(buffer, (int)capacity, file) != NULL;
    fclose(file);
    if (success) {
        buffer[strcspn(buffer, "\r\n")] = '\0';
    }
    return success;
}

static int parse_cache_size(const char *text, size_t *size)
{
    errno = 0;
    char *suffix;
    unsigned long long value = strtoull(text, &suffix, 10);
    if (errno != 0 || suffix == text) {
        return 0;
    }

    if (*suffix == 'K' || *suffix == 'k') {
        value *= 1024;
        ++suffix;
    } else if (*suffix == 'M' || *suffix == 'm') {
        value *= 1024 * 1024;
        ++suffix;
    }

    if (*suffix != '\0' || value > SIZE_MAX) {
        return 0;
    }
    *size = (size_t)value;
    return 1;
}

static int detect_cache_topology(struct cache_topology *topology)
{
    topology->cpu = sched_getcpu();
    if (topology->cpu < 0) {
        return 0;
    }

    for (int index = 0; index < MAX_CACHE_INDEX; ++index) {
        char path[PATH_MAX];
        char level_text[32];
        char type[32];
        char size_text[32];
        char line_text[32];

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/level",
                 topology->cpu, index);
        if (!read_text_file(path, level_text, sizeof(level_text))) {
            continue;
        }

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/type",
                 topology->cpu, index);
        if (!read_text_file(path, type, sizeof(type))) {
            continue;
        }

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/size",
                 topology->cpu, index);
        if (!read_text_file(path, size_text, sizeof(size_text))) {
            continue;
        }

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/coherency_line_size",
                 topology->cpu, index);
        if (!read_text_file(path, line_text, sizeof(line_text))) {
            continue;
        }

        int level = atoi(level_text);
        size_t cache_bytes;
        size_t line_bytes;
        if (!parse_cache_size(size_text, &cache_bytes)
            || !parse_cache_size(line_text, &line_bytes)) {
            continue;
        }

        if (line_bytes > topology->line_bytes) {
            topology->line_bytes = line_bytes;
        }
        if (level == 1 && strcmp(type, "Data") == 0) {
            topology->l1_bytes = cache_bytes;
        } else if (level == 2 && strcmp(type, "Unified") == 0) {
            topology->l2_bytes = cache_bytes;
        } else if (level == 3 && strcmp(type, "Unified") == 0) {
            topology->llc_bytes = cache_bytes;
        }
    }

    return topology->line_bytes != 0
        && topology->l1_bytes != 0
        && topology->l2_bytes != 0
        && topology->llc_bytes != 0;
}

static size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static int initialize_pointer_cycle(struct pointer_cycle *cycle,
                                    size_t requested_bytes,
                                    size_t line_bytes)
{
    cycle->bytes = round_up(requested_bytes, line_bytes);
    cycle->lines = cycle->bytes / line_bytes;
    if (cycle->lines < 2) {
        return 0;
    }

    if (posix_memalign((void **)&cycle->allocation, TARGET_PAGE_BYTES,
                       cycle->bytes) != 0) {
        cycle->allocation = NULL;
        return 0;
    }
    memset(cycle->allocation, 0, cycle->bytes);

    size_t *order = malloc(cycle->lines * sizeof(*order));
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
        uintptr_t *node = (uintptr_t *)(cycle->allocation
                                        + order[i] * line_bytes);
        *node = (uintptr_t)(cycle->allocation + order[next] * line_bytes);
    }
    cycle->cursor = (uintptr_t)(cycle->allocation + order[0] * line_bytes);
    free(order);
    return 1;
}

static void free_pointer_cycle(struct pointer_cycle *cycle)
{
    free(cycle->allocation);
    cycle->allocation = NULL;
    cycle->cursor = 0;
    cycle->bytes = 0;
    cycle->lines = 0;
}

static void traverse_cycle(struct pointer_cycle *cycle, size_t accesses)
{
    uintptr_t cursor = cycle->cursor;
    for (size_t i = 0; i < accesses; ++i) {
        cursor = *(volatile uintptr_t *)cursor;
    }
    cycle->cursor = cursor;
    traversal_sink = cursor;
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

static void warm_target(volatile int *target)
{
    (void)*target;
    memory_fence();
}

static void prepare_state(int state,
                          volatile int *target,
                          volatile int *translation_helper,
                          struct pointer_cycle *l1_eviction,
                          struct pointer_cycle *l2_eviction)
{
    if (state == 0) {
        (void)*target;
    } else if (state == 1) {
        warm_target(target);
        traverse_cycle(l1_eviction, l1_eviction->lines);
    } else if (state == 2) {
        warm_target(target);
        traverse_cycle(l2_eviction, l2_eviction->lines);
    } else {
        memory_flush((const void *)target);
        memory_fence();
    }

    (void)*translation_helper;
}

static int collect_prepared_samples(struct timing_record *records,
                                    size_t *record_index,
                                    size_t samples,
                                    const struct cache_topology *topology)
{
    static const struct prepared_state states[PREPARED_STATE_COUNT] = {
        {"l1", 1},
        {"l2", 2},
        {"llc", 3},
        {"memory", 0},
    };
    struct pointer_cycle l1_eviction = {0};
    struct pointer_cycle l2_eviction = {0};
    unsigned char *target_page = NULL;
    int success = 0;

    if (posix_memalign((void **)&target_page, TARGET_PAGE_BYTES,
                       TARGET_PAGE_BYTES) != 0) {
        goto cleanup;
    }
    memset(target_page, 0, TARGET_PAGE_BYTES);

    if (!initialize_pointer_cycle(&l1_eviction, topology->l1_bytes * 4,
                                  topology->line_bytes)
        || !initialize_pointer_cycle(&l2_eviction, topology->l2_bytes * 4,
                                     topology->line_bytes)) {
        goto cleanup;
    }

    traverse_cycle(&l1_eviction, l1_eviction.lines);
    traverse_cycle(&l2_eviction, l2_eviction.lines);

    volatile int *target = (volatile int *)(target_page + TARGET_OFFSET_BYTES);
    volatile int *translation_helper =
        (volatile int *)(target_page + TRANSLATION_OFFSET_BYTES);

    for (size_t sample = 0; sample < samples; ++sample) {
        size_t order[PREPARED_STATE_COUNT] = {0, 1, 2, 3};
        shuffle_indices(order, PREPARED_STATE_COUNT);

        for (size_t position = 0; position < PREPARED_STATE_COUNT; ++position) {
            size_t state = order[position];
            prepare_state((int)state, target, translation_helper,
                          &l1_eviction, &l2_eviction);

            records[*record_index] = (struct timing_record) {
                .mode = "prepared",
                .label = states[state].label,
                .expected_level = states[state].expected_level,
                .working_set_bytes = 0,
                .sample = sample,
                .latency_ticks = cache_level_measure_ticks(target),
            };
            ++*record_index;
        }
    }
    success = 1;

cleanup:
    free_pointer_cycle(&l1_eviction);
    free_pointer_cycle(&l2_eviction);
    free(target_page);
    return success;
}

static int collect_working_set_samples(struct timing_record *records,
                                       size_t *record_index,
                                       size_t samples,
                                       const struct cache_topology *topology)
{
    const char *labels[WORKING_SET_COUNT] = {
        "fits_l1",
        "fits_l2",
        "fits_llc",
        "exceeds_llc",
    };
    size_t sizes[WORKING_SET_COUNT] = {
        topology->l1_bytes / 2,
        topology->l2_bytes / 2,
        topology->llc_bytes / 2,
        topology->llc_bytes * 4,
    };

    for (size_t set = 0; set < WORKING_SET_COUNT; ++set) {
        struct pointer_cycle cycle = {0};
        if (!initialize_pointer_cycle(&cycle, sizes[set],
                                      topology->line_bytes)) {
            return 0;
        }

        traverse_cycle(&cycle, cycle.lines);
        for (size_t sample = 0; sample < samples; ++sample) {
            uintptr_t next;
            uint64_t latency = cache_level_measure_pointer_ticks(
                (volatile uintptr_t *)cycle.cursor, &next);
            cycle.cursor = next;

            records[*record_index] = (struct timing_record) {
                .mode = "working_set",
                .label = labels[set],
                .expected_level = -1,
                .working_set_bytes = cycle.bytes,
                .sample = sample,
                .latency_ticks = latency,
            };
            ++*record_index;
        }
        free_pointer_cycle(&cycle);
    }
    return 1;
}

static int write_records(const char *path,
                         const struct cache_topology *topology,
                         const struct timing_record *records,
                         size_t record_count)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return 0;
    }

    fprintf(file,
            "cpu,l1_bytes,l2_bytes,llc_bytes,line_bytes,mode,label,"
            "expected_level,working_set_bytes,sample,latency_ticks\n");
    for (size_t i = 0; i < record_count; ++i) {
        const struct timing_record *record = &records[i];
        fprintf(file, "%d,%zu,%zu,%zu,%zu,%s,%s,%d,%zu,%zu,%" PRIu64 "\n",
                topology->cpu,
                topology->l1_bytes,
                topology->l2_bytes,
                topology->llc_bytes,
                topology->line_bytes,
                record->mode,
                record->label,
                record->expected_level,
                record->working_set_bytes,
                record->sample,
                record->latency_ticks);
    }

    int success = fclose(file) == 0;
    return success;
}

static int parse_samples(const char *text, size_t *samples)
{
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 2
        || value > SIZE_MAX) {
        return 0;
    }
    *samples = (size_t)value;
    return 1;
}

int main(int argc, char **argv)
{
    const char *output_path = "cache_level_timings.csv";
    size_t samples = DEFAULT_SAMPLES;
    struct cache_topology topology = {0};
    struct timing_record *records = NULL;
    int return_code = EXIT_FAILURE;

    if (argc > 3) {
        fprintf(stderr, "usage: %s [output.csv] [samples-per-state]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2) {
        output_path = argv[1];
    }
    if (argc == 3 && !parse_samples(argv[2], &samples)) {
        fprintf(stderr, "invalid sample count: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    if (!pin_to_current_cpu()) {
        perror("sched_setaffinity");
        goto cleanup;
    }
    if (!detect_cache_topology(&topology)) {
        fprintf(stderr, "could not detect cache topology for the current CPU\n");
        goto cleanup;
    }

    if (samples > SIZE_MAX / (PREPARED_STATE_COUNT + WORKING_SET_COUNT)
        || samples * (PREPARED_STATE_COUNT + WORKING_SET_COUNT)
            > SIZE_MAX / sizeof(*records)) {
        fprintf(stderr, "sample count is too large\n");
        goto cleanup;
    }

    size_t record_capacity = samples
        * (PREPARED_STATE_COUNT + WORKING_SET_COUNT);
    records = calloc(record_capacity, sizeof(*records));
    if (records == NULL) {
        perror("calloc");
        goto cleanup;
    }

    printf("CPU %d: L1d=%zu KiB, L2=%zu KiB, LLC=%zu KiB, line=%zu bytes\n",
           topology.cpu,
           topology.l1_bytes / 1024,
           topology.l2_bytes / 1024,
           topology.llc_bytes / 1024,
           topology.line_bytes);
    printf("Collecting %zu samples per state\n", samples);
    if (!warm_up_cpu()) {
        perror("clock_gettime");
        goto cleanup;
    }

    size_t record_count = 0;
    if (!collect_prepared_samples(records, &record_count, samples, &topology)
        || !collect_working_set_samples(records, &record_count, samples,
                                        &topology)) {
        fprintf(stderr, "could not allocate timing buffers\n");
        goto cleanup;
    }

    if (!write_records(output_path, &topology, records, record_count)) {
        perror(output_path);
        goto cleanup;
    }

    printf("Wrote %zu measurements to %s\n", record_count, output_path);
    return_code = EXIT_SUCCESS;

cleanup:
    free(records);
    return return_code;
}
