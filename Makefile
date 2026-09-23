UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
	CC ?= clang
else
	CC ?= gcc
endif

CPU ?= 0
TASKSET ?= taskset

CPPFLAGS ?=
CFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra
EARLY_TASK_CFLAGS := -O2
GATE_CFLAGS := -O1 -falign-functions=8

TASK_NAMES := task1 task2 task3 task4 task5 task6
EARLY_PROGRAMS := task1.out task2.out
GATE_PROGRAMS := task3.out task4.out task5.out task6.out calibrate_threshold.out
TASK_PROGRAMS := $(addsuffix .out,$(TASK_NAMES))
PROGRAMS := $(TASK_PROGRAMS) calibrate_threshold.out
COMMON_DEPENDENCIES := got.c got.h consts.h arch_primitives.h

.DELETE_ON_ERROR:

.PHONY: all tasks calibration clean run-all run-calibration \
	$(TASK_NAMES) $(addprefix run-,$(TASK_NAMES))

all: calibration tasks

tasks: $(TASK_PROGRAMS)

calibration: calibrate_threshold.out

$(TASK_NAMES): %: %.out

$(EARLY_PROGRAMS): BUILD_CFLAGS := $(EARLY_TASK_CFLAGS)
$(GATE_PROGRAMS): BUILD_CFLAGS := $(GATE_CFLAGS)

$(PROGRAMS): %.out: %.c $(COMMON_DEPENDENCIES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $(BUILD_CFLAGS) $< got.c \
		$(LDFLAGS) $(LDLIBS) -o $@

$(addprefix run-,$(TASK_NAMES)): run-%: %.out
	$(TASKSET) -c $(CPU) ./$<

run-calibration: calibrate_threshold.out
	$(TASKSET) -c $(CPU) ./calibrate_threshold.out

run-all: all
	$(TASKSET) -c $(CPU) ./calibrate_threshold.out
	$(TASKSET) -c $(CPU) ./task1.out
	$(TASKSET) -c $(CPU) ./task2.out
	$(TASKSET) -c $(CPU) ./task3.out
	$(TASKSET) -c $(CPU) ./task4.out
	$(TASKSET) -c $(CPU) ./task5.out
	$(TASKSET) -c $(CPU) ./task6.out

clean:
	$(RM) $(PROGRAMS)
