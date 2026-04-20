# 运维仪表盘示例

这个示例集使用两份带注解的 CSV 文件和一组 Flux 脚本，目标是覆盖更真实的主机监控查询场景：

- 通过 `csv.from(file: ...)` 从磁盘加载 CPU 和内存样本
- 在同一份内置数据集上验证多种查询组合

## 包含的脚本

- `query.flux`：`filter + aggregateWindow + group + join + yield`
- `cpu_top_windows.flux`：`aggregateWindow + sort + limit`
- `cpu_distinct_hosts.flux`：`keep + distinct + yield`
- `cpu_gap_fill.flux`：`aggregateWindow(createEmpty) + fill(usePrevious) + yield`
- `cpu_elapsed_by_host.flux`：`group + sort + elapsed + yield`
- `cpu_usage_difference.flux`：`group + sort + difference + yield`
- `cpu_usage_derivative.flux`：`group + sort + derivative + yield`
- `latest_two_cpu_windows.flux`：`keep + tail(offset) + yield`
- `host_usage_pivot.flux`：`aggregateWindow + union + pivot + yield`
- `monthly_cpu_calendar.flux`：`filter + aggregateWindow(1mo) + yield`
- `monthly_cpu_calendar_offset.flux`：`aggregateWindow(1mo, offset: 15d) + yield`
- `cpu_period_overlap.flux`：`aggregateWindow(every != period) + yield`
- `cpu_negative_period.flux`：`aggregateWindow(period: -40s) + yield`
- `cpu_selector_sparse_windows.flux`：`aggregateWindow(createEmpty) + selector drop-empty + yield`
- `fleet_usage_union.flux`：`keep + rename + set + union + sort + limit`
- `edge1_cpu_rollup.flux`：`filter + reduce`
- `latest_west_cpu.flux`：`filter + sort + last`

## 运行方式

在仓库根目录执行：

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_distinct_hosts.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_elapsed_by_host.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_difference.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_usage_derivative.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar_offset.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_period_overlap.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_negative_period.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_selector_sparse_windows.flux
```

导出注解 CSV：

```bash
./bazel-bin/cpp/pl/flux/flux --output-format csv cpp/pl/flux/examples/ops_dashboard/query.flux
```

再试几种不同形态：

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
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/monthly_cpu_calendar_offset.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_period_overlap.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_negative_period.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/cpu_selector_sparse_windows.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/fleet_usage_union.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/edge1_cpu_rollup.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/latest_west_cpu.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --list-results cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --result latest_east_mem cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
```

## 结果检查点

`host_health` 结果应包含 `us-east` 中 `edge-1` 的两个 1 分钟窗口：

- `2024-05-01T10:01:00Z`，CPU 均值 `72`，内存均值 `63`
- `2024-05-01T10:02:00Z`，CPU 均值 `82`，内存均值 `68`

这个结果现在会显式使用 join 后的重命名列，例如 `_value_cpu` 和 `_value_mem`。脚本里也会先 `group(columns: ["host", "region"])`，把 CPU / MEM 两侧原本不同的 `_measurement` group key 去掉之后再 join，这和官方 Flux 的使用方式一致。

`monthly_cpu_calendar` 结果应包含 `us-east` 中 `edge-1` 的两个 UTC 月历窗口：

- `2024-01-01T00:00:00Z` 到 `2024-02-01T00:00:00Z`，CPU 均值 `60`
- `2024-02-01T00:00:00Z` 到 `2024-03-01T00:00:00Z`，CPU 均值 `77`

`monthly_cpu_calendar_offset` 结果应把月窗口整体后移 15 天：

- `2024-01-16T00:00:00Z` 到 `2024-02-16T00:00:00Z`，CPU 均值 `10`
- `2024-02-16T00:00:00Z` 到 `2024-03-16T00:00:00Z`，CPU 均值 `30`

`cpu_period_overlap` 结果应产生 20 秒步长、40 秒跨度的重叠窗口：

- `2024-01-01T00:00:00Z` 到 `2024-01-01T00:00:40Z`，`_value` 为 `4`
- `2024-01-01T00:00:20Z` 到 `2024-01-01T00:01:00Z`，`_value` 为 `4`

`cpu_negative_period` 结果应产生 `_start` 晚于 `_stop` 的回看窗口：

- `2024-01-01T00:00:40Z` 到 `2024-01-01T00:00:00Z`，`_value` 为 `4`
- `2024-01-01T00:01:00Z` 到 `2024-01-01T00:00:20Z`，`_value` 为 `4`

`cpu_distinct_hosts` 结果应保留每个主机第一次出现的 CPU 样本：

- `us-east` 的 `edge-1`，`_value` 为 `70`
- `us-west` 的 `edge-2`，`_value` 为 `91`

`cpu_gap_fill` 结果应为 `edge-1` 创建 30 秒窗口，并把上一个值填到空窗口：

- `2024-05-01T10:00:30Z`，`_value` 为 `70`
- `2024-05-01T10:01:00Z`，`_value` 为 `74`
- `2024-05-01T10:01:30Z`，`_value` 为 `82`

`cpu_selector_sparse_windows` 结果即便开启 `createEmpty: true`，也应只保留非空 selector 窗口：

- `2024-01-01T00:00:00Z` 到 `2024-01-01T00:01:00Z`，`_value` 为 `10`
- `2024-01-01T00:02:00Z` 到 `2024-01-01T00:03:00Z`，`_value` 为 `30`

`cpu_elapsed_by_host` 结果应按主机分别报告采样间隔秒数：

- `us-east` 的 `edge-1` 在 `2024-05-01T10:00:40Z`，`elapsed` 为 `30`
- `us-east` 的 `edge-1` 在 `2024-05-01T10:01:05Z`，`elapsed` 为 `25`
- `us-west` 的 `edge-2` 在 `2024-05-01T10:01:10Z`，`elapsed` 为 `50`

`cpu_usage_difference` 结果应按主机分别报告样本差值：

- `us-east` 的 `edge-1` 在 `2024-05-01T10:00:40Z`，`_value` 为 `4`
- `us-east` 的 `edge-1` 在 `2024-05-01T10:01:05Z`，`_value` 为 `8`
- `us-west` 的 `edge-2` 在 `2024-05-01T10:01:10Z`，`_value` 为 `-4`

`cpu_usage_derivative` 结果应按主机分别报告每秒速率：

- `us-east` 的 `edge-1` 在 `2024-05-01T10:00:40Z`，`_value` 为 `0.133333333333333`
- `us-east` 的 `edge-1` 在 `2024-05-01T10:01:05Z`，`_value` 为 `0.32`
- `us-west` 的 `edge-2` 在 `2024-05-01T10:01:10Z`，`_value` 为 `-0.08`

`latest_two_cpu_windows` 结果应保留最终 CPU 样本之前的两行：

- `us-east` 的 `edge-1` 在 `2024-05-01T10:01:05Z`，`_value` 为 `82`
- `us-west` 的 `edge-2` 在 `2024-05-01T10:00:20Z`，`_value` 为 `91`

`host_usage_pivot` 结果应把一分钟 CPU / 内存窗口透视成宽表：

- `2024-05-01T10:01:00Z`，`us-east` 的 `edge-1`，`cpu` 为 `72`，`mem` 为 `63`
- `2024-05-01T10:01:00Z`，`us-west` 的 `edge-2`，`cpu` 为 `91`，`mem` 为 `72`
- `2024-05-01T10:02:00Z`，`us-east` 的 `edge-1`，`cpu` 为 `82`，`mem` 为 `68`
- `2024-05-01T10:02:00Z`，`us-west` 的 `edge-2`，`cpu` 为 `87`，`mem` 为 `75`

## 关于 `group`

这组示例里有不少脚本依赖 `group` 的真实语义，例如 `cpu_elapsed_by_host.flux`、`cpu_usage_difference.flux`、`cpu_usage_derivative.flux`。当前运行时里，`group()` 已经会真正按 group key 重分表，而不是只给行打 `_group` 标签，因此这些脚本里的 `sort`、`elapsed`、`difference`、`derivative`、`first`、`last`、`count` 都会按主机各自的逻辑表分别运行。
