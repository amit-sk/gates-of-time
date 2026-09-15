Undergraduate's project in micro-architectural attacks and weird gates.

# Gates of Time

## Table of contents

- [Calibration](#calibration)
  - [Compiling and Running](#compiling-and-running)
- [Basic operations](#basic-operations)
- [Testing Mispredictions](#testing-mispredictions)

## Calibration

`calibrate_threshold.c` measures 10,000 cached and 10,000 flushed loads on x86
to estimate `CACHE_HIT_THRESHOLD`.

Set your run's constant in `consts.h`: `test` in `got.c` should return 1 when
`elapsed < CACHE_HIT_THRESHOLD`, otherwise 0, using the same timing sequence as
`measure`. Calibration covers warm L1 hits; validate LLC hits when testing gates.

```bash
cc -O2 -std=c11 calibrate_threshold.c -o calibrate_threshold.out
taskset -c 0 ./calibrate_threshold.out
```

Pin calibration and gate experiments to the same available CPU. Recalibrate after
changing the machine, CPU, or timing sequence.

Example output from a run pinned to CPU 0:

```text
Cached:  median=42, p95=44 ticks
Flushed: p05=206, median=210 ticks
#define CACHE_HIT_THRESHOLD (125)
With elapsed < threshold: cached errors=0.00%, flushed errors=0.00%
```

- `median`: middle timing; `p95` and `p05`: 95th and 5th percentiles.
- `ticks`: timestamp-counter units, including measurement overhead.
- Threshold: midpoint between cached `p95` and flushed `p05`
  (`(44 + 206) / 2 = 125` here).
- Errors: percentages of cached samples classified as uncached and flushed
  samples classified as cached.

If the percentile ranges overlap, the program uses the median midpoint; if the
medians do not separate, it exits without a threshold.

## Basic operations

`got.c` implements `clear` (evict a line), `set` (cache a line without fences),
and `test` (classify its cache state by load timing).

`task1.c` calls `init()` to ramp up the CPU, then runs 10,000 trials, randomly
choosing `set` or `clear` before checking the result with `test`.

```bash
cc -O2 -std=c11 task1.c got.c -o task1.out
taskset -c 0 ./task1.out
```

The output reports success separately for cached and uncached trials:

```text
Cached: 100.00% success (5054/5054 correct)
Uncached: 100.00% success (4946/4946 correct)
```

## Testing Mispredictions

`task2.c` ramps up the CPU with `init()` and runs 10,000 trials each for `branch1`
and `branch2`. Each trial trains twice with `zero`, flushes `one`, then calls with
`one` and checks the output's cache state. `branch2` adds a 256-iteration loop to
make recent branch history more consistent.

```bash
cc -O2 -std=gnu11 task2.c got.c -o task2.out
taskset -c 0 ./task2.out
```

GNU C mode supports the supplied `asm` syntax. Use the same CPU as calibration.

Example output (rates vary by machine and run):

```text
branch1: 0.01% mispredictions detected (1/10000 cached after inverse condition)
branch2: 0.09% mispredictions detected (9/10000 cached after inverse condition)
```
