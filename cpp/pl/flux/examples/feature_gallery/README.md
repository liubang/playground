# Feature Gallery

This gallery is a coverage-oriented set of runnable Flux examples for the
current `cpp/pl/flux` runtime. The examples use a small but realistic service
operations dataset plus a few targeted inline tables.

Run from the repository root:

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/scalar_basics.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/function_pipelines.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/csv_raw_alerts.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/inspection_helpers.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/table_shape_ops.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/selection_and_reduce.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/join_union_pivot.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/fill_distinct_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/time_math.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/aggregatewindow_advanced.flux
```

Included examples:

- `scalar_basics.flux`: scalar builtins, arrays/objects, member/index access,
  `exists`, conditionals, and string interpolation
- `function_pipelines.flux`: `option`, user-defined functions, default params,
  pipe params, regex filters, `map`, and `set`
- `csv_raw_alerts.flux`: `import "csv"` with `csv.from(..., mode: "raw")`
- `inspection_helpers.flux`: `columns`, `keys`, `findColumn`, `findRecord`, and
  repeated annotated CSV metadata blocks
- `table_shape_ops.flux`: `filter`, `duplicate`, `rename`, `set`, `map`, `drop`,
  `keep`, `sort`, `limit`, and `tail`
- `selection_and_reduce.flux`: `group`, `count`, `first`, `last`, and `reduce`
- `join_union_pivot.flux`: `aggregateWindow + join`, `union`, and `pivot`
- `fill_distinct_windows.flux`: `aggregateWindow(createEmpty)`, `fill`, and
  `distinct`
- `time_math.flux`: `range`, `elapsed`, `difference`, and `derivative`
- `aggregatewindow_advanced.flux`: the richer `aggregateWindow` parameter
  combinations: `column`, fixed `offset`, custom aggregate functions, `period`,
  negative `period`, `timeSrc`, `timeDst`, named-zone `location`, calendar
  `offset`, and selector empty-window behavior

Coverage map for the current builtin surface:

- `len`, `string`, `contains`, `sum`, `mean`, `min`, `max`:
  `scalar_basics.flux`
- `from`: `function_pipelines.flux`, `aggregatewindow_advanced.flux`
- `csv.from`: `csv_raw_alerts.flux`, `inspection_helpers.flux`,
  `table_shape_ops.flux`, `selection_and_reduce.flux`, `join_union_pivot.flux`,
  `fill_distinct_windows.flux`, `time_math.flux`
- `columns`, `keys`, `findColumn`, `findRecord`: `inspection_helpers.flux`
- `range`, `filter`, `map`: `function_pipelines.flux`, `csv_raw_alerts.flux`,
  `table_shape_ops.flux`, `time_math.flux`
- `limit`, `tail`, `keep`, `drop`, `rename`, `duplicate`, `set`:
  `table_shape_ops.flux`
- `reduce`, `sort`, `group`, `count`, `first`, `last`:
  `selection_and_reduce.flux`
- `pivot`, `fill`, `distinct`, `union`, `join`, `aggregateWindow`, `yield`:
  `join_union_pivot.flux`, `fill_distinct_windows.flux`,
  `aggregatewindow_advanced.flux`
- `elapsed`, `difference`, `derivative`: `time_math.flux`

Language/runtime features covered here:

- `option` assignment
- user-defined functions and closures
- default arguments
- pipe parameters
- object updates with `with`
- `if ... then ... else ...`
- `exists`
- regex match and string interpolation

Data files:

- `site_ops.annotated.csv`: service CPU and memory samples for three regions
- `service_counters.annotated.csv`: monotonic counters with a reset
- `alerts.raw.csv`: raw CSV ingestion example
- `multi_block.annotated.csv`: repeated annotated metadata/header blocks
