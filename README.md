Undergraduate's project in micro-architectural attacks and weird gates.

# Gates of Time

## Table of contents

- [Calibration](#calibration)
- [Basic operations](#basic-operations)
- [Testing Mispredictions](#testing-mispredictions)
- [NOT gate](#not-gate)
- [NAND gate](#nand-gate)

## Calibration

`calibrate_threshold.c` ramps up the CPU, measures 10,000 cached and 10,000
flushed loads to estimate `CACHE_HIT_THRESHOLD`, sweeps the NOT delay, and tests
the configured NAND2 delay for all four input combinations.
`test()` in `got.c` treats `elapsed < CACHE_HIT_THRESHOLD` as cached.

The NOT sweep calibrates `NOT_MISPREDICTION_DELAY_STEPS`. It tests delays from 0 to
64 with 1,000 trials per input state and prints the value that gives the best
accuracy for the weaker of the cached and uncached input cases.

NAND2 uses the separate `NAND2_MISPREDICTION_DELAY_STEPS` constant. See below.

Copy the printed `#define` values into `consts.h`, then recompile and rerun.

```bash
cc -O1 -falign-functions=8 -std=gnu11 calibrate_threshold.c got.c -o calibrate_threshold.out && taskset -c 0 ./calibrate_threshold.out
```

Pin calibration and gate experiments to the same available CPU. Recalibrate after
changing the machine, CPU, timing sequence, or gate code.

Example output from a run pinned to CPU 0:

```text
=== Cache hit threshold calibration ===
Cached:  median=42, p95=44 ticks
Flushed: p05=200, median=204 ticks
Cache errors: cached=0.00%, flushed=0.00%
#define CACHE_HIT_THRESHOLD (122)

=== Misprediction delay steps calibration ===
NOT delay best results (28 steps): accuracy cached=100.00%, uncached=90.60%
#define NOT_MISPREDICTION_DELAY_STEPS (28)

=== NAND2 delay steps calibration ===
NAND2 delay results (30 steps): joint accuracy uncached/uncached=98.80%, uncached/cached=98.60%, cached/uncached=97.80%, cached/cached=100.00%
#define NAND2_MISPREDICTION_DELAY_STEPS (30)
```

- `median`: middle timing; `p95` and `p05`: 95th and 5th percentiles.
- `ticks`: timestamp-counter units, including measurement overhead.
- Threshold: midpoint between cached `p95` and flushed `p05`
  (`(44 + 200) / 2 = 122` here).
- Cache errors: percentages of cached samples classified as uncached and flushed
  samples classified as cached.
- NOT accuracy: correct outputs for cached and uncached **inputs**. A cached input
  should leave the output uncached; an uncached input should cache the output.
If the percentile ranges overlap, the program uses the median midpoint; if the
medians do not separate, it exits without a threshold.

The NOT sweep always prints its highest-scoring candidate and does not require a
minimum accuracy. The runtime sweep can differ from compiled `not()`.

### NAND2 Misprediction Delay Calibration

NAND2 uses the separate `NAND2_MISPREDICTION_DELAY_STEPS` constant. Since its
result is sensitive to compiled code placement, `calibrate_nand2_delay.py`
rebuilds and runs the actual Task 5 binary for every candidate using temporary
source copies. This command compares 28--30 with five runs per value on CPU 0:

```bash
python3 calibrate_nand2_delay.py --min-steps 28 --max-steps 30 --runs 5 --cpu 0
```

Example mean joint accuracies:

```text
Steps       UU       UC       CU       CC    Minimum
   28    83.26%   81.99%   84.60%   99.99%     81.99%
   29    83.86%   85.13%   82.32%   99.99%     82.32%
   30    86.22%   84.33%   84.02%  100.00%     84.02%

Best NAND2 delay: 30 steps
#define NAND2_MISPREDICTION_DELAY_STEPS (30)
```

`UU`, `UC`, `CU`, and `CC` identify the two input cache states. `Minimum` is the
lowest mean accuracy among them and is used to select the delay.

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
cc -O1 -falign-functions=8 -std=gnu11 task3.c got.c -o task3.out && taskset -c 0 ./task3.out
```

Use `-O1 -falign-functions=8` for the gate tests because optimization and
function placement strongly affect the speculative behavior. The current
`NOT_MISPREDICTION_DELAY_STEPS` value of 28 was calibrated with these flags.

Expect three output blocks. Example output (rates vary by machine and run):

```text
not:
  Cached input: 100.00% accuracy (50049/50050 correct)
  Uncached input: 91.91% accuracy (45907/49950 correct)
imul_not:
  Cached input: 99.97% accuracy (50037/50050 correct)
  Uncached input: 0.07% accuracy (34/49950 correct)
imul_not2:
  Cached input: 100.00% accuracy (50049/50050 correct)
  Uncached input: 0.54% accuracy (268/49950 correct)
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
Inputs (uncached, uncached), expected output cached: 96.52% accuracy (24008/24874 correct)
Inputs (uncached, cached), expected output cached: 94.33% accuracy (23589/25006 correct)
Inputs (cached, uncached), expected output cached: 95.45% accuracy (23897/25035 correct)
Inputs (cached, cached), expected output uncached: 100.00% accuracy (25084/25085 correct)
```

Cached represents logical 1 and uncached represents logical 0. NAND should
produce a cached output unless both inputs are cached. Each row shows the
percentage and count of trials in which `test()` observed that expected state.
`nand()` retains its separately calibrated `NAND_MISPREDICTION_DELAY_STEPS`
value of 50; all gate tests use the same compiler and CPU-pinning procedure.
