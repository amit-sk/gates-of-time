Undergraduate's project in micro-architectural attacks and weird gates.

# Gates of Time

## Table of contents

- [Calibration](#calibration)
- [Basic operations](#basic-operations)
- [Testing Mispredictions](#testing-mispredictions)
- [NOT gate](#not-gate)
- [NAND gate](#nand-gate)

## Calibration

`calibrate_threshold.c` ramps up the CPU, then measures 10,000 cached and 10,000
flushed loads to estimate `CACHE_HIT_THRESHOLD`. It also sweeps delays from 0 to
64 steps, testing 1,000 trials per input state for each delay.

Copy the printed `#define` values into `consts.h`, then recompile and rerun.
`test()` treats `elapsed < CACHE_HIT_THRESHOLD` as cached. Calibration covers
warm L1 hits; validate LLC hits when testing gates.

```bash
cc -O2 -std=c11 calibrate_threshold.c got.c -o calibrate_threshold.out && taskset -c 0 ./calibrate_threshold.out
```

Pin calibration and gate experiments to the same available CPU. Recalibrate after
changing the machine, CPU, timing sequence, or gate code.

Example output from a run pinned to CPU 0:

```text
=== Cache hit threshold calibration ===
Cached:  median=58, p95=62 ticks
Flushed: p05=230, median=234 ticks
Cache errors: cached=0.00%, flushed=0.00%
#define CACHE_HIT_THRESHOLD (146)

=== Misprediction delay steps calibration ===
NOT delay best results (50 steps): accuracy cached=100.00%, uncached=98.40%
#define MISPREDICTION_DELAY_STEPS (50)
Recompile and rerun to validate the delay.
```

- `median`: middle timing; `p95` and `p05`: 95th and 5th percentiles.
- `ticks`: timestamp-counter units, including measurement overhead.
- Threshold: midpoint between cached `p95` and flushed `p05`
  (`(62 + 230) / 2 = 146` here).
- Cache errors: percentages of cached samples classified as uncached and flushed
  samples classified as cached.
- NOT accuracy: correct outputs for cached and uncached **inputs**. A cached input
  should leave the output uncached; an uncached input should cache the output.
- Best delay: the runtime sweep candidate with the highest accuracy for its
  weaker input state; ties favor higher total accuracy.

If the percentile ranges overlap, the program uses the median midpoint; if the
medians do not separate, it exits without a threshold.

A delay must reach 95% accuracy for both input states. The printed delay keeps
the configured value if the actual `not()` passes; otherwise it uses the best
sweep candidate. If neither passes, the candidate is still printed and the
program exits with failure. The runtime sweep can differ from compiled `not()`.

## Basic operations

`got.c` implements `clear` (evict a line), `set` (cache a line without fences),
and `test` (classify its cache state by load timing).

`task1.c` calls `init()` to ramp up the CPU, then runs 10,000 trials, randomly
choosing `set` or `clear` before checking the result with `test`.

```bash
cc -O2 -std=c11 task1.c got.c -o task1.out && taskset -c 0 ./task1.out
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
cc -O2 -std=gnu11 task2.c got.c -o task2.out && taskset -c 0 ./task2.out
```

GNU C mode supports the supplied `asm` syntax. Use the same CPU as calibration.

Example output (rates vary by machine and run):

```text
branch1: 0.01% mispredictions detected (1/10000 cached after inverse condition)
branch2: 0.09% mispredictions detected (9/10000 cached after inverse condition)
```

## NOT gate

`task3.c` tests `not()`, `imul_not()`, and `imul_not2()` with 100,000 random
input cache states. It ramps up the CPU, trains the predictor, and reports
cached- and uncached-input accuracy for each implementation.

```bash
cc -O1 -std=gnu11 task3.c got.c -o task3.out && taskset -c 0 ./task3.out
```

Use `-O1` because optimization changes the branch-history loop and can strongly
affect the result.

Expect three output blocks. Example output (rates vary by machine and run):

```text
not:
  Cached input: 100.00% accuracy (49826/49826 correct)
  Uncached input: 91.00% accuracy (45658/50174 correct)
imul_not:
  Cached input: 100.00% accuracy (49825/49826 correct)
  Uncached input: 0.03% accuracy (17/50174 correct)
imul_not2:
  Cached input: 99.99% accuracy (49821/49826 correct)
  Uncached input: 0.05% accuracy (24/50174 correct)
```

Accuracy is reported separately for each input state: cached inputs should leave
the output uncached, while uncached inputs should cache the output (hence - not).

## NAND gate

`task4.c` ramps up the CPU and runs 100,000 randomized tests of `nand()`. It
trains the predictor before each invocation and reports accuracy separately for
all four input cache-state combinations.

```bash
cc -O1 -falign-functions=8 -std=gnu11 task4.c got.c -o task4.out && taskset -c 0 ./task4.out
```

The alignment flag `-falign-functions=8` gives `nand()` a branch-predictor-friendly placement on the
tested machine. Results may change after modifying the source, compiler, or CPU.

Example output (rates vary by machine and run):

```text
Inputs (uncached, uncached), expected output cached: 80.63% accuracy (20124/24958 correct)
Inputs (uncached, cached), expected output cached: 77.95% accuracy (19259/24708 correct)
Inputs (cached, uncached), expected output cached: 79.07% accuracy (20002/25295 correct)
Inputs (cached, cached), expected output uncached: 99.59% accuracy (24937/25039 correct)
```

Cached represents logical 1 and uncached represents logical 0. NAND should
produce a cached output unless both inputs are cached. Each row shows the
percentage and count of trials in which `test()` observed that expected state.
