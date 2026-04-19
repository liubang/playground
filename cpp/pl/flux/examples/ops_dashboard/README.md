# Ops Dashboard Example

This example uses two annotated CSV files with realistic host metrics and a Flux
script set that:

- loads CPU and memory samples from disk with `csv.from(file: ...)`
- covers several query combinations against the same checked-in dataset

Included scripts:

- `query.flux`: `filter + aggregateWindow + join + yield`
- `cpu_top_windows.flux`: `aggregateWindow + sort + limit`
- `cpu_distinct_hosts.flux`: `keep + distinct + yield`
- `cpu_gap_fill.flux`: `aggregateWindow(createEmpty) + fill(usePrevious) + yield`
- `cpu_elapsed_by_host.flux`: `group + sort + elapsed + yield`
- `cpu_usage_difference.flux`: `group + sort + difference + yield`
- `cpu_usage_derivative.flux`: `group + sort + derivative + yield`
- `latest_two_cpu_windows.flux`: `keep + tail(offset) + yield`
- `host_usage_pivot.flux`: `aggregateWindow + union + pivot + yield`
- `monthly_cpu_calendar.flux`: `filter + aggregateWindow(1mo) + yield`
- `fleet_usage_union.flux`: `keep + rename + set + union + sort + limit`
- `edge1_cpu_rollup.flux`: `filter + reduce`
- `latest_west_cpu.flux`: `filter + sort + last`

Run it from the repository root:

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_distinct_hosts.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_elapsed_by_host.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_difference.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_derivative.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar.flux
```

Export the same result as annotated CSV:

```bash
./bazel-bin/cpp/pl/flux/flux --annotated-csv cpp/pl/flux/examples/ops_dashboard/query.flux
```

Try another query shape from the same directory:

```bash
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_top_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_distinct_hosts.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_gap_fill.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_elapsed_by_host.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_difference.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_derivative.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/latest_two_cpu_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/host_usage_pivot.flux
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

The `cpu_distinct_hosts` result should keep the first CPU sample for each host:

- `edge-1` in `us-east` with `_value` `70`
- `edge-2` in `us-west` with `_value` `91`

The `cpu_gap_fill` result should create 30-second windows for `edge-1` and
carry the previous value across empty windows:

- `2024-05-01T10:00:30Z` with `_value` `70`
- `2024-05-01T10:01:00Z` with `_value` `74`
- `2024-05-01T10:01:30Z` with `_value` `82`

The `cpu_elapsed_by_host` result should report per-host sample spacing in
seconds:

- `2024-05-01T10:00:40Z` for `edge-1` in `us-east` with `elapsed` `30`
- `2024-05-01T10:01:05Z` for `edge-1` in `us-east` with `elapsed` `25`
- `2024-05-01T10:01:10Z` for `edge-2` in `us-west` with `elapsed` `50`

The `cpu_usage_difference` result should report per-host sample deltas:

- `2024-05-01T10:00:40Z` for `edge-1` in `us-east` with `_value` `4`
- `2024-05-01T10:01:05Z` for `edge-1` in `us-east` with `_value` `8`
- `2024-05-01T10:01:10Z` for `edge-2` in `us-west` with `_value` `-4`

The `cpu_usage_derivative` result should report per-host per-second rates:

- `2024-05-01T10:00:40Z` for `edge-1` in `us-east` with `_value` `0.133333333333333`
- `2024-05-01T10:01:05Z` for `edge-1` in `us-east` with `_value` `0.32`
- `2024-05-01T10:01:10Z` for `edge-2` in `us-west` with `_value` `-0.08`

The `latest_two_cpu_windows` result should keep the two rows just before the
final CPU sample:

- `2024-05-01T10:01:05Z` for `edge-1` in `us-east` with `_value` `82`
- `2024-05-01T10:00:20Z` for `edge-2` in `us-west` with `_value` `91`

The `host_usage_pivot` result should pivot one-minute CPU and memory windows
into wide rows:

- `2024-05-01T10:01:00Z` for `edge-1` in `us-east` with `cpu` `72` and `mem` `63`
- `2024-05-01T10:01:00Z` for `edge-2` in `us-west` with `cpu` `91` and `mem` `72`
- `2024-05-01T10:02:00Z` for `edge-1` in `us-east` with `cpu` `82` and `mem` `68`
- `2024-05-01T10:02:00Z` for `edge-2` in `us-west` with `cpu` `87` and `mem` `75`
