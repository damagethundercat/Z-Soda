# Performance QA Harness

`zsoda_perf_harness` is a synthetic-frame QA/perf executable for quick local checks and CI-like runs.

It measures:
- `ms/frame` (`total_ms`, `avg_ms_per_frame`)
- render status counts (`cache_hit`, `inference`, `fallback_tiled`, `fallback_downscaled`, `safe_output`)
- cache hit behavior using repeated synthetic frame hashes

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Quick Runs

Benchmark mode (compares no-cache vs cache scenarios):

```bash
./build/tests/zsoda_perf_harness --mode benchmark
```

Stability mode (1000+ frame long run, CI-friendly defaults):

```bash
./build/tests/zsoda_perf_harness --mode stability
```

Default stability settings use `1200` frames at `64x36` with unique frame hashes for a fast repeated-inference loop.

## Useful Options

```bash
./build/tests/zsoda_perf_harness \
  --mode benchmark \
  --frames 300 \
  --width 320 --height 180 \
  --cache-cycle 16 \
  --warmup 20 \
  --spin-iters 0
```

- `--cache-cycle 0`: all unique frame hashes (no intentional cache reuse)
- `--cache-cycle N (>0)`: hashes repeat every `N` frames, producing cache hits when cache is enabled
- `--quiet`: one-line summary output per scenario

## CTest Integration

The harness is registered as two tests:

```bash
ctest --test-dir build -R zsoda_perf_benchmark --output-on-failure
ctest --test-dir build -R zsoda_perf_stability --output-on-failure
```

or run both:

```bash
ctest --test-dir build -R zsoda_perf_ --output-on-failure
```

## Pass/Fail Behavior

The executable exits non-zero on QA failures, for example:
- `safe_output` occurred
- `fallback` path was used unexpectedly
- benchmark mode produced no cache hits in cache scenario
- stability mode used fewer than 1000 frames
