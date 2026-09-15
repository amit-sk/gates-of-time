Undergraduate's project in micro-architectural attacks and weird gates.

# Gates of Time

## Calibration

`calibrate_threshold.c` measures 10,000 cached and 10,000 flushed loads on x86
to estimate `CACHE_HIT_THRESHOLD`.

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

Set your run's constant in `consts.h`: `test` in `got.c` should return 1 when
`elapsed < CACHE_HIT_THRESHOLD`, otherwise 0, using the same timing sequence as
`measure`. Calibration covers warm L1 hits; validate LLC hits when testing gates.

### Compiling and Running

```bash
cc -O2 -std=c11 calibrate_threshold.c -o calibrate_threshold.out
taskset -c 0 ./calibrate_threshold.out
```

Pin calibration and gate experiments to the same available CPU. Recalibrate after
changing the machine, CPU, or timing sequence.
