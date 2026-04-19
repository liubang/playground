# Ops Dashboard Example

This example uses two annotated CSV files with realistic host metrics and a Flux
script set that:

- loads CPU and memory samples from disk with `csv.from(file: ...)`
- covers several query combinations against the same checked-in dataset

Included scripts:

- `query.flux`: `filter + aggregateWindow + join + yield`
- `cpu_top_windows.flux`: `aggregateWindow + sort + limit`
- `monthly_cpu_calendar.flux`: `filter + aggregateWindow(1mo) + yield`
- `fleet_usage_union.flux`: `keep + rename + set + union + sort + limit`
- `edge1_cpu_rollup.flux`: `filter + reduce`
- `latest_west_cpu.flux`: `filter + sort + last`

Run it from the repository root:

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar.flux
```

Export the same result as annotated CSV:

```bash
./bazel-bin/cpp/pl/flux/flux --annotated-csv cpp/pl/flux/examples/ops_dashboard/query.flux
```

Try another query shape from the same directory:

```bash
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_top_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/fleet_usage_union.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/edge1_cpu_rollup.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/latest_west_cpu.flux
```

The final `host_health` result should include two 1-minute windows for
`edge-1` in `us-east`:

- `2024-05-01T10:01:00Z` with CPU mean `72` and memory mean `63`
- `2024-05-01T10:02:00Z` with CPU mean `82` and memory mean `68`

The `monthly_cpu_calendar` result should include two UTC calendar windows for
`edge-1` in `us-east`:

- `2024-01-01T00:00:00Z` to `2024-02-01T00:00:00Z` with CPU mean `60`
- `2024-02-01T00:00:00Z` to `2024-03-01T00:00:00Z` with CPU mean `77`
