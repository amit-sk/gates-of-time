Undergraduate's project in micro-architectural attacks and weird gates.

# Gates of Time

## Table of contents

- [Building and running](#building-and-running)
- [Calibration](#calibration)
- [Basic operations](#basic-operations)
- [Testing Mispredictions](#testing-mispredictions)
- [NOT gate](#not-gate)
- [NAND gate](#nand-gate)
- [NAND2 gate](#nand2-gate)
- [Additional gates and half adder](#additional-gates-and-half-adder)
- [Full and three-bit adders](#full-and-three-bit-adders)
- [Cache-level timing study](#cache-level-timing-study)

`example_results.txt` contains the complete output from a representative
`make run-all` execution pinned to CPU 0.

## Building and running

Build the calibration program and all task programs:

```bash
make
```

Build and run the complete suite on CPU 0:

```bash
make run-all
```

Use targets such as `make run-task3` to run an individual task and
`make run-calibration` to run calibration. Set `CPU` to select another available
CPU, for example `make CPU=2 run-all`. The Makefile supplies the required
optimization and function-layout flags for each program.

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
make run-calibration
```

Pin calibration and gate experiments to the same available CPU. Recalibrate after
changing the machine, CPU, timing sequence, or gate code.

Example output from a run pinned to CPU 0:

```text
=== Cache hit threshold calibration ===
Cached:  median=60, p95=74 ticks
Flushed: p05=236, median=304 ticks
Cache errors: cached=0.12%, flushed=0.00%
#define CACHE_HIT_THRESHOLD (155)

=== Misprediction delay steps calibration ===
NOT delay best results (23 steps): accuracy cached=100.00%, uncached=38.80%
#define NOT_MISPREDICTION_DELAY_STEPS (23)

=== NAND2 delay steps calibration ===
NAND2 delay results (27 steps): joint accuracy uncached/uncached=91.20%, uncached/cached=89.90%, cached/uncached=90.40%, cached/cached=100.00%
#define NAND2_MISPREDICTION_DELAY_STEPS (27)
```

- `median`: middle timing; `p95` and `p05`: 95th and 5th percentiles.
- `ticks`: timestamp-counter units, including measurement overhead.
- Threshold: midpoint between cached `p95` and flushed `p05`
  (`(74 + 236) / 2 = 155` here).
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
source copies. This command compares 26--28 with five runs per value on CPU 0:

```bash
python3 calibrate_nand2_delay.py --min-steps 26 --max-steps 28 --runs 5 --cpu 0
```

The script prints mean joint accuracy for `UU`, `UC`, `CU`, and `CC`, which
identify the two input cache states. It selects the delay with the highest
minimum accuracy among the four states.

## Basic operations

`got.c` implements `clear` (evict a line), `set` (cache a line without fences),
and `test` (classify its cache state by load timing).

`task1.c` calls `init()` to ramp up the CPU, then runs 10,000 trials, randomly
choosing `set` or `clear` before checking the result with `test`.

```bash
make run-task1
```

The output reports success separately for cached and uncached trials:

```text
Cached: 100.00% success (4979/4979 correct)
Uncached: 100.00% success (5021/5021 correct)
```

## Testing Mispredictions

`task2.c` ramps up the CPU with `init()` and runs 10,000 trials each for `branch1`
and `branch2`. Each trial trains twice with `zero`, flushes `one`, then calls with
`one` and checks the output's cache state. `branch2` adds a 256-iteration loop to
make recent branch history more consistent.

```bash
make run-task2
```

GNU C mode supports the supplied `asm` syntax. Use the same CPU as calibration.

Example output (rates vary by machine and run):

```text
branch1: 0.00% mispredictions detected (0/10000 cached after inverse condition)
branch2: 0.65% mispredictions detected (65/10000 cached after inverse condition)
```

## NOT gate

`task3.c` tests `not()`, `imul_not()`, and `imul_not2()` with 100,000 random
input cache states. It ramps up the CPU, trains the predictor, and reports
cached- and uncached-input accuracy for each implementation.

```bash
make run-task3
```

Use `-O1 -falign-functions=8 -fno-toplevel-reorder` for the gate tests because
optimization and function placement strongly affect the speculative behavior.
The Makefile supplies these flags for Tasks 3--7 and calibration.

Expect three output blocks. Example output (rates vary by machine and run):

```text
not:
  Cached input: 100.00% accuracy (50270/50270 correct)
  Uncached input: 98.27% accuracy (48868/49730 correct)
imul_not:
  Cached input: 100.00% accuracy (50269/50270 correct)
  Uncached input: 0.01% accuracy (6/49730 correct)
imul_not2:
  Cached input: 99.99% accuracy (50266/50270 correct)
  Uncached input: 0.11% accuracy (53/49730 correct)
```

Accuracy is reported separately for each input state: cached inputs should leave
the output uncached, while uncached inputs should cache the output (hence - not).

## NAND gate

`task4.c` ramps up the CPU and runs 100,000 randomized tests of `nand()`. It
trains the predictor before each invocation and reports accuracy separately for
all four input cache-state combinations.

```bash
make run-task4
```

The alignment and ordering flags give `nand()` a branch-predictor-friendly
placement on the tested machine. Results may change after modifying the source,
compiler, or CPU.

Example output (rates vary by machine and run):

```text
Inputs (uncached, uncached), expected output cached: 97.59% accuracy (24457/25060 correct)
Inputs (uncached, cached), expected output cached: 92.36% accuracy (23128/25040 correct)
Inputs (cached, uncached), expected output cached: 97.09% accuracy (24240/24967 correct)
Inputs (cached, cached), expected output uncached: 99.98% accuracy (24929/24933 correct)
```

Cached represents logical 1 and uncached represents logical 0. NAND should
produce a cached output unless both inputs are cached. Each row shows the
percentage and count of trials in which `test()` observed that expected state.
`nand()` retains its separately calibrated `NAND_MISPREDICTION_DELAY_STEPS`
value of 50; all gate tests use the same compiler and CPU-pinning procedure.

## NAND2 gate

`nand2()` implements NAND with two output cache lines. `task5.c` runs 100,000
randomized tests, trains the predictor four times before each measured call, and
reports results for all four input combinations.

```bash
make run-task5
```

Both outputs should be cached unless both inputs are cached. `Output 1 correct`
and `Output 2 correct` measure each output separately. `Both correct` is the
joint gate success rate. `Outputs identical` only measures agreement, so both
outputs can agree while both are wrong.

Example output with four training calls and the current 27-step delay:

```text
Inputs (uncached, uncached), expected outputs cached:
  Output 1 correct: 87.89% (22026/25060)
  Output 2 correct: 97.53% (24440/25060)
  Both correct: 87.81% (22006/25060)
  Outputs identical: 90.21% (22606/25060)
Inputs (uncached, cached), expected outputs cached:
  Output 1 correct: 85.77% (21477/25040)
  Output 2 correct: 97.33% (24371/25040)
  Both correct: 85.61% (21437/25040)
  Outputs identical: 88.12% (22066/25040)
Inputs (cached, uncached), expected outputs cached:
  Output 1 correct: 86.32% (21551/24967)
  Output 2 correct: 97.45% (24331/24967)
  Both correct: 86.23% (21530/24967)
  Outputs identical: 88.70% (22145/24967)
Inputs (cached, cached), expected outputs uncached:
  Output 1 correct: 100.00% (24933/24933)
  Output 2 correct: 100.00% (24933/24933)
  Both correct: 100.00% (24933/24933)
  Outputs identical: 100.00% (24933/24933)
```

These rates are sensitive to the delay, predictor training, compiled code
placement, CPU, and other system activity.

`nand2()` is placed after the other gates and aligned to 128 bytes. The build
uses `-fno-toplevel-reorder` to preserve that source order. The half adder uses
two looped NAND2 training calls; the standalone Task 5 tester uses four.

## Additional gates and half adder

`task6.c` separately tests FAN2, AND, OR, and the half adder with 100,000
randomized trials per test. FAN2 and the half adder report each output as well
as joint correctness.

```bash
make run-task6
```

Example output:

```text
FAN2 gate:
Uncached input, expected outputs uncached:
  Output 1 correct: 99.99% (50011/50014)
  Output 2 correct: 99.99% (50008/50014)
  Both correct: 99.99% (50008/50014)
  Outputs identical: 99.99% (50011/50014)
Cached input, expected outputs cached:
  Output 1 correct: 92.35% (46162/49986)
  Output 2 correct: 97.93% (48951/49986)
  Both correct: 91.24% (45607/49986)
  Outputs identical: 92.20% (46087/49986)

AND gate:
Inputs (uncached, uncached), expected output uncached: 99.99% (24888/24890)
Inputs (uncached, cached), expected output uncached: 99.99% (25109/25111)
Inputs (cached, uncached), expected output uncached: 99.99% (24913/24916)
Inputs (cached, cached), expected output cached: 85.36% (21412/25083)

OR gate:
Inputs (uncached, uncached), expected output uncached: 100.00% (24966/24966)
Inputs (uncached, cached), expected output cached: 87.43% (21783/24914)
Inputs (cached, uncached), expected output cached: 90.06% (22622/25118)
Inputs (cached, cached), expected output cached: 87.18% (21797/25002)

Half adder:
Inputs (uncached, uncached), expected sum uncached, carry uncached:
  Sum correct: 99.95% (25098/25110)
  Carry correct: 95.81% (24057/25110)
  Both correct: 95.76% (24045/25110)
Inputs (uncached, cached), expected sum cached, carry uncached:
  Sum correct: 90.55% (22578/24933)
  Carry correct: 95.70% (23861/24933)
  Both correct: 90.55% (22577/24933)
Inputs (cached, uncached), expected sum cached, carry uncached:
  Sum correct: 90.32% (22709/25143)
  Carry correct: 95.65% (24050/25143)
  Both correct: 90.31% (22707/25143)
Inputs (cached, cached), expected sum uncached, carry cached:
  Sum correct: 99.70% (24739/24814)
  Carry correct: 95.35% (23661/24814)
  Both correct: 95.35% (23660/24814)
```

For FAN2, `Both correct` requires both copies to match the input. For the half
adder, it requires both sum and carry to match the expected logical values.

## Full and three-bit adders

`task7.c` tests the full adder for all eight combinations of two input bits and
a carry-in. It then tests `adder3_impl()` with 100,000 random pairs of three-bit
values. The three-bit result is computed modulo eight, so its final carry is
discarded.

```bash
make run-task7
```

Example output:

```text
Full adder:
Inputs (uncached, uncached), carry-in uncached, expected sum uncached, carry uncached:
  Sum correct: 99.52% (12251/12310)
  Carry correct: 92.31% (11363/12310)
  Both correct: 91.96% (11320/12310)
Inputs (uncached, uncached), carry-in cached, expected sum cached, carry uncached:
  Sum correct: 68.00% (8589/12630)
  Carry correct: 92.09% (11631/12630)
  Both correct: 67.03% (8466/12630)
Inputs (uncached, cached), carry-in uncached, expected sum cached, carry uncached:
  Sum correct: 64.96% (8245/12693)
  Carry correct: 91.93% (11669/12693)
  Both correct: 64.83% (8229/12693)
Inputs (uncached, cached), carry-in cached, expected sum uncached, carry cached:
  Sum correct: 96.35% (12092/12550)
  Carry correct: 71.22% (8938/12550)
  Both correct: 70.37% (8832/12550)
Inputs (cached, uncached), carry-in uncached, expected sum cached, carry uncached:
  Sum correct: 64.88% (8185/12616)
  Carry correct: 92.55% (11676/12616)
  Both correct: 64.76% (8170/12616)
Inputs (cached, uncached), carry-in cached, expected sum uncached, carry cached:
  Sum correct: 95.73% (11761/12286)
  Carry correct: 70.53% (8665/12286)
  Both correct: 69.68% (8561/12286)
Inputs (cached, cached), carry-in uncached, expected sum uncached, carry cached:
  Sum correct: 99.53% (12377/12436)
  Carry correct: 76.33% (9492/12436)
  Both correct: 76.18% (9474/12436)
Inputs (cached, cached), carry-in cached, expected sum cached, carry cached:
  Sum correct: 66.14% (8253/12479)
  Carry correct: 73.58% (9182/12479)
  Both correct: 60.42% (7540/12479)

Three-bit adder:
  Exact sum correct: 57.31% (57307/100000)
  Sum bit 0 correct: 82.81% (82808/100000)
  Sum bit 1 correct: 74.85% (74854/100000)
  Sum bit 2 correct: 73.17% (73165/100000)
```

`Both correct` requires the full adder's sum and carry to be correct in the same
trial. `Exact sum correct` requires all three result bits to match; the per-bit
rates show where errors accumulate across the composed circuit.

## Cache-level timing study

`cache_level_timings.c` measures whether load latency can distinguish L1, L2,
LLC, and uncached memory and produces calibration data for `cacheLevel()`. It
pins itself to one CPU, warms up the CPU, and writes every measurement to
`cache_level_timings.csv`.

Run the measurement and analysis with:

```bash
make run-cache-level-timings && make analyze-cache-levels
```

The experiment collects two kinds of samples. Prepared-state samples load one
target directly for L1, walk four times the L1 or L2 capacity to attempt to
leave it in L2 or LLC, or flush it for the uncached state. Random working-set
samples use dependent pointer cycles of 24 KiB, 640 KiB, 12 MiB, and 96 MiB on
the current machine. The dependency prevents ordinary hardware prefetching.

`analyze_cache_levels.py` fits three ordered thresholds on the even-numbered
prepared samples, choosing the thresholds with the fewest classification
errors. It evaluates those thresholds on the unused odd-numbered samples. The
current representative run produced:

| Intended state | Median | p95 | Validation accuracy |
| --- | ---: | ---: | ---: |
| L1 | 58 ticks | 76 ticks | 94.36% |
| L2 | 78 ticks | 116 ticks | 97.04% |
| LLC | 160 ticks | 406 ticks | 80.20% |
| Uncached | 340 ticks | 652 ticks | 100.00% |

The fitted thresholds classify measurements at most 72 ticks as L1, 73--132
as L2, 133--240 as LLC, and values above 240 as uncached. Overall validation
agreement was 92.90%.

This percentage is agreement with the state the test attempted to prepare, not
independently verified cache-level accuracy. Loading and flushing give strong
L1 and uncached labels, but capacity walks only approximate L2 and LLC state.
The largest error source is the overlap between the LLC tail and uncached
memory. Thresholds also move between runs, so they should be calibrated for the
same machine, CPU, and timing sequence used by `cacheLevel()`.

`cache_level_histogram.svg` contains two panels. In the prepared-state panel,
the horizontal axis is serialized load time in timestamp-counter ticks, the
vertical axis is the fraction of samples in each two-tick bin, and the dashed
lines are the fitted thresholds. Blue represents L1, green L2, orange LLC, and
red uncached memory. Overlap between distributions shows where classification
is ambiguous. The working-set panel shows random access latency as the
footprint grows; these footprint sizes are not ground-truth cache-level labels
and are not included in the accuracy calculation. Random accesses also include
TLB misses and shared-cache contention.

The plot stops at the 99.5th percentile so rare interruptions do not compress
the useful portion of the graph. Its legends count values beyond that limit,
while the raw CSV retains every sample. The reported ticks include timer and
fence overhead and are not bare load cycles. `cache_level_thresholds.csv`
contains the selected thresholds and fitting and validation error counts.
