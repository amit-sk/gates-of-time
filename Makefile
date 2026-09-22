CC ?= cc
CPU ?= 0
TASKSET ?= taskset

CPPFLAGS ?=
CFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra
EARLY_TASK_CFLAGS := -O2
GATE_CFLAGS := -O1 -falign-functions=8 -fno-toplevel-reorder

TASK_NAMES := task1 task2 task3 task4 task5 task6 task7
EARLY_PROGRAMS := task1.out task2.out
GATE_PROGRAMS := task3.out task4.out task5.out task6.out task7.out calibrate_threshold.out
CACHE_LEVEL_PROGRAM := cache_level_timings.out
TASK_PROGRAMS := $(addsuffix .out,$(TASK_NAMES))
PROGRAMS := $(TASK_PROGRAMS) calibrate_threshold.out
COMMON_DEPENDENCIES := got.c got.h consts.h arch_primitives.h
CACHE_LEVEL_CSV ?= cache_level_timings.csv
CACHE_LEVEL_SUMMARY ?= cache_level_thresholds.csv
CACHE_LEVEL_PLOT ?= cache_level_histogram.svg
CACHE_LEVEL_SAMPLES ?= 5000

.DELETE_ON_ERROR:

.PHONY: all tasks calibration cache-level-tools clean run-all run-calibration \
	run-cache-level-timings analyze-cache-levels \
	$(TASK_NAMES) $(addprefix run-,$(TASK_NAMES))

all: calibration tasks

tasks: $(TASK_PROGRAMS)

calibration: calibrate_threshold.out

cache-level-tools: $(CACHE_LEVEL_PROGRAM)

$(TASK_NAMES): %: %.out

$(EARLY_PROGRAMS): BUILD_CFLAGS := $(EARLY_TASK_CFLAGS)
$(GATE_PROGRAMS): BUILD_CFLAGS := $(GATE_CFLAGS)

$(PROGRAMS): %.out: %.c $(COMMON_DEPENDENCIES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $(BUILD_CFLAGS) $< got.c \
		$(LDFLAGS) $(LDLIBS) -o $@

$(CACHE_LEVEL_PROGRAM): cache_level_timings.c cache_level.c cache_level.h arch_primitives.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -O2 \
		cache_level_timings.c cache_level.c $(LDFLAGS) $(LDLIBS) -o $@

$(addprefix run-,$(TASK_NAMES)): run-%: %.out
	$(TASKSET) -c $(CPU) ./$<

run-calibration: calibrate_threshold.out
	$(TASKSET) -c $(CPU) ./calibrate_threshold.out

run-cache-level-timings: $(CACHE_LEVEL_PROGRAM)
	$(TASKSET) -c $(CPU) ./$(CACHE_LEVEL_PROGRAM) \
		$(CACHE_LEVEL_CSV) $(CACHE_LEVEL_SAMPLES)

analyze-cache-levels:
	python3 analyze_cache_levels.py $(CACHE_LEVEL_CSV) \
		--summary $(CACHE_LEVEL_SUMMARY) --plot $(CACHE_LEVEL_PLOT)

run-all: all
	$(TASKSET) -c $(CPU) ./calibrate_threshold.out
	$(TASKSET) -c $(CPU) ./task1.out
	$(TASKSET) -c $(CPU) ./task2.out
	$(TASKSET) -c $(CPU) ./task3.out
	$(TASKSET) -c $(CPU) ./task4.out
	$(TASKSET) -c $(CPU) ./task5.out
	$(TASKSET) -c $(CPU) ./task6.out
	$(TASKSET) -c $(CPU) ./task7.out

clean:
	$(RM) $(PROGRAMS) $(CACHE_LEVEL_PROGRAM)
