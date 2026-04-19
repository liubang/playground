# Flux Benchmarking

This directory keeps the local synthetic benchmarks for the current in-memory
Flux runtime. The goal is not to produce a perfectly isolated microbenchmark,
but to give us a repeatable way to compare behavior as we optimize the engine.

## What It Covers

The benchmark suite currently exercises four representative query shapes:

- `linear`: `csv.from |> filter |> map |> limit`
- `sort`: `csv.from |> sort |> limit`
- `agg`: `csv.from |> group |> aggregateWindow |> yield`
- `join`: two-table `join(..., on: ["_time", "host"]) |> limit`

These cases are intentionally chosen to cover:

- a mostly linear pipeline
- a full-table reorder
- a stateful aggregation path
- the heavier multi-table join path, which is now backed by a hash index

## Run

Build the binary first:

```bash
bazel build //cpp/pl/flux:flux
```

Generate synthetic annotated CSV inputs and run the default benchmark matrix:

```bash
python3 cpp/pl/flux/benchmark/generate_benchmark_data.py
python3 cpp/pl/flux/benchmark/run_benchmarks.py
```

The runner prints JSON so it is easy to compare before and after a change:

```json
[
  {
    "case": "linear:100000",
    "rc": 0,
    "seconds": 2.003
  }
]
```

## Data Shape

The generated benchmark inputs live under `/tmp/flux_bench` by default:

- `metrics_100000.annotated.csv`
- `metrics_500000.annotated.csv`
- `metrics_1000000.annotated.csv`
- `join_left_2000.annotated.csv`
- `join_right_2000.annotated.csv`
- `join_left_5000.annotated.csv`
- `join_right_5000.annotated.csv`

The large metric datasets use annotated CSV with the columns:

- `result`
- `table`
- `_time`
- `host`
- `region`
- `_value`

## Current Baseline

These baseline results were collected locally on 2026-04-19 after the
current round of runtime optimizations, including the hash-indexed `join`
path, indexed `aggregateWindow` bucket lookup, and row-pointer reuse across
selection/reordering operators:

| Case | Input | Time |
| --- | --- | ---: |
| `linear` | 100k rows | 0.086s |
| `linear` | 500k rows | 0.368s |
| `linear` | 1M rows | 0.770s |
| `sort` | 100k rows | 0.081s |
| `sort` | 500k rows | 0.442s |
| `sort` | 1M rows | 1.082s |
| `agg` | 100k rows | 0.448s |
| `agg` | 500k rows | 2.233s |
| `agg` | 1M rows | 4.465s |
| `join` | 2000 x 2000 rows | 0.061s |
| `join` | 5000 x 5000 rows | 0.336s |

## How To Use It

When we change runtime behavior, the usual workflow should be:

1. Run the benchmark suite on the current branch.
2. Save the JSON output from `run_benchmarks.py`.
3. Make the runtime change.
4. Run the same benchmark suite again.
5. Compare latency changes for `linear`, `sort`, `agg`, and `join`.

If we add new heavy operators such as better `pivot` coverage, hash-based join,
or streaming readers, we should extend this directory with a new benchmark case
instead of replacing the existing ones.
