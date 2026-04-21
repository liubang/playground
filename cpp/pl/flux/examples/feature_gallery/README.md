# 功能示例集

这是一组偏覆盖率导向的可运行 Flux 示例，用来展示 `cpp/pl/flux` 当前运行时已经支持的能力。示例使用了一份小而真实的服务运维数据集，以及少量内联表。

## 运行方式

在仓库根目录执行：

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/scalar_basics.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/function_pipelines.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/array_watchlist_join.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/csv_raw_alerts.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/inspection_helpers.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/table_shape_ops.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/selection_and_reduce.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/join_union_pivot.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/fill_distinct_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/time_math.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/aggregatewindow_advanced.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/task_driven_rollup.flux
```

## 示例说明

- `scalar_basics.flux`：标量 builtin、数组/对象、成员/索引访问、`exists`、条件表达式、字符串插值
- `function_pipelines.flux`：`option`、用户函数、默认参数、pipe 参数、正则过滤、`map`、`set`
- `array_watchlist_join.flux`：`array.concat`、`array.filter`、`array.map`、`array.from`，以及数组配置驱动的 `join`
- `csv_raw_alerts.flux`：`import "csv"` 与 `csv.from(..., mode: "raw")`
- `inspection_helpers.flux`：`columns`、`keys`、`findColumn`、`findRecord`，以及重复 annotated CSV metadata block
- `table_shape_ops.flux`：`filter`、`duplicate`、`rename`、`set`、`map`、`drop`、`keep`、`sort`、`limit`、`tail`
- `selection_and_reduce.flux`：`group`、`count`、`first`、`last`、`reduce`
- `join_union_pivot.flux`：`aggregateWindow + group + join`、`union`、`pivot`
- `fill_distinct_windows.flux`：`aggregateWindow(createEmpty)`、`fill`、`distinct`
- `time_math.flux`：`range`、`elapsed`、`difference`、`derivative`
- `aggregatewindow_advanced.flux`：更完整的 `aggregateWindow` 参数组合，包括 `column`、固定时长 `offset`、自定义聚合函数、`period`、负 `period`、`timeSrc`、`timeDst`、命名时区 `location`、日历窗口 `offset`、selector 空窗口行为
- `task_driven_rollup.flux`：`option task = {...}` 驱动的窗口查询，结合 block-body helper、对象返回、嵌套条件和多主机 rollup

## builtin 覆盖映射

- `len`、`string`、`contains`、`sum`、`mean`、`min`、`max`：
  `scalar_basics.flux`
- `from`：
  `function_pipelines.flux`、`aggregatewindow_advanced.flux`
- `array.from`、`array.concat`、`array.filter`、`array.map`：
  `array_watchlist_join.flux`
- `csv.from`：
  `array_watchlist_join.flux`、`csv_raw_alerts.flux`、`inspection_helpers.flux`、`table_shape_ops.flux`、`selection_and_reduce.flux`、`join_union_pivot.flux`、`fill_distinct_windows.flux`、`time_math.flux`、`task_driven_rollup.flux`
- `columns`、`keys`、`findColumn`、`findRecord`：
  `inspection_helpers.flux`
- `range`、`filter`、`map`：
  `function_pipelines.flux`、`csv_raw_alerts.flux`、`table_shape_ops.flux`、`time_math.flux`
- `limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`：
  `table_shape_ops.flux`
- `reduce`、`sort`、`group`、`count`、`first`、`last`：
  `selection_and_reduce.flux`
- `pivot`、`fill`、`distinct`、`union`、`join`、`aggregateWindow`、`yield`：
  `array_watchlist_join.flux`、`join_union_pivot.flux`、`fill_distinct_windows.flux`、`aggregatewindow_advanced.flux`、`task_driven_rollup.flux`
- `elapsed`、`difference`、`derivative`：
  `time_math.flux`
- `option`、block-body helper、对象返回、嵌套条件：
  `scalar_basics.flux`、`function_pipelines.flux`、`task_driven_rollup.flux`

## 这里覆盖的语言 / 运行时特性

- `option` 赋值
- 用户自定义函数与闭包
- 默认参数
- pipe 参数
- 使用 `with` 的对象更新
- `if ... then ... else ...`
- `exists`
- 正则匹配与字符串插值

## 数据文件

- `data/site_ops.annotated.csv`：三大区域的服务 CPU / 内存样本
- `data/service_counters.annotated.csv`：带一次 reset 的单调计数器
- `data/alerts.raw.csv`：raw CSV 读取示例
- `data/multi_block.annotated.csv`：带重复 annotated metadata/header block 的输入

## 关于 `group`

`selection_and_reduce.flux` 现在展示的是更新后的官方风格 `group` 语义：

- `group()` 会真正把输入重新划分为多张逻辑表
- `count()`、`first()`、`last()` 会按每张逻辑表分别计算
- `mode: "by"` 和 `mode: "except"` 都已支持

因此这个示例更适合拿来检查我们当前的 `group`、selector 和聚合语义是否与官方 Flux 保持一致。

`join_union_pivot.flux` 则对应更新后的 `join` 语义：

- join 只会比较 group key 实例相同的逻辑表
- 不同 measurement 的聚合结果会先显式 `group(columns: ["host", "region"])` 再 join
- 重复非 `on` 列会重命名成 `_value_cpu`、`_value_mem`、`region_cpu`、`region_mem` 这类官方风格列名
