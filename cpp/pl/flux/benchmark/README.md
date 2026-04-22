# Flux 基准测试

这个目录保存了当前内存内 Flux 运行时的本地合成基准测试。目标不是做完全隔离的微基准，而是给我们一个可重复的比较方法，用来观察引擎优化前后的变化。

如果想看这轮优化是怎么一步步做出来的，可以配合阅读 [OPTIMIZATION_LOG.md](/Volumes/workspace/liubang/playground/cpp/pl/flux/benchmark/OPTIMIZATION_LOG.md)。

## 覆盖内容

当前基准集覆盖五类有代表性的查询形态：

- `linear`：`csv.from |> filter |> map |> limit`
- `sort`：`csv.from |> sort |> limit`
- `agg`：`csv.from |> group |> aggregateWindow |> yield`
- `group`：`csv.from |> group |> count |> yield`
- `array`：`findColumn + array.map/filter/contains/reduce/any/all`
- `pivot`：`csv.from |> pivot |> limit`
- `join`：双表 `join(..., on: ["_time", "host"]) |> limit`

选择这四类 case 的目的分别是覆盖：

- 近似线性的轻管道
- 全表重排
- 带状态的聚合路径
- 纯 group 重分表路径
- 数组 package 的真实 helper 组合路径
- 宽表整形路径
- 更重的多表 `join` 路径，目前已经由哈希索引支撑

## 运行方式

先构建二进制：

```bash
bazel build //cpp/pl/flux:flux
```

生成合成 annotated CSV 输入，并执行默认基准矩阵：

```bash
python3 cpp/pl/flux/benchmark/generate_benchmark_data.py
python3 cpp/pl/flux/benchmark/run_benchmarks.py
```

runner 会输出 JSON，便于在修改前后直接对比：

```json
[
  {
    "case": "linear:100000",
    "rc": 0,
    "seconds": 2.003
  }
]
```

## 数据形态

生成的基准输入默认位于 `/tmp/flux_bench`：

- `metrics_100000.annotated.csv`
- `metrics_500000.annotated.csv`
- `metrics_1000000.annotated.csv`
- `pivot_100000.annotated.csv`
- `pivot_500000.annotated.csv`
- `pivot_1000000.annotated.csv`
- `join_left_2000.annotated.csv`
- `join_right_2000.annotated.csv`
- `join_left_5000.annotated.csv`
- `join_right_5000.annotated.csv`

大规模指标数据集使用 annotated CSV，列包括：

- `result`
- `table`
- `_time`
- `host`
- `region`
- `_value`

## 当前基线

下面这组基线会随着运行时实现变化持续更新。当前这版是在 2026-04-22 本地采集的，包含：

- 基于哈希索引的 `join`
- `aggregateWindow` 的按逻辑表/按 group 聚合路径，以及 tumbling window 快路径
- `group()` 的单次扫描分组 key / `_group` 构造路径
- `pivot()` / `join()` 中几处字符串 key 构造与集合判断的减法
- selection / reordering 算子中的行指针复用
- `array.contains` / `array.reduce` / `array.any` / `array.all` 所在的数组 helper 路径

注意：这套 runner 更适合做“本地同机前后对比”，并不追求冷启动隔离；如果你连续运行多轮，结果会明显受热缓存影响。

| Case | 输入规模 | 时间 |
| --- | --- | ---: |
| `linear` | 100k rows | 0.085s |
| `linear` | 500k rows | 0.369s |
| `linear` | 1M rows | 0.737s |
| `sort` | 100k rows | 0.077s |
| `sort` | 500k rows | 0.409s |
| `sort` | 1M rows | 1.003s |
| `agg` | 100k rows | 0.179s |
| `agg` | 500k rows | 0.937s |
| `agg` | 1M rows | 1.877s |
| `group` | 100k rows | 0.127s |
| `group` | 500k rows | 0.742s |
| `group` | 1M rows | 1.534s |
| `array` | 100k rows | 0.092s |
| `array` | 500k rows | 0.441s |
| `array` | 1M rows | 0.880s |
| `pivot` | 100k rows | 0.090s |
| `pivot` | 500k rows | 0.347s |
| `pivot` | 1M rows | 0.674s |
| `join` | 2000 x 2000 rows | 0.009s |
| `join` | 5000 x 5000 rows | 0.013s |

## 如何使用

当我们修改运行时行为时，推荐流程如下：

1. 在当前分支先跑一遍基准。
2. 保存 `run_benchmarks.py` 的 JSON 输出。
3. 修改运行时实现。
4. 用同样的输入规模再跑一遍。
5. 对比 `linear`、`sort`、`agg`、`group`、`array`、`pivot`、`join` 的延迟变化。

如果后续新增更重的算子、更多 join 形态，或者真正的流式 reader，建议在这里新增新的 benchmark case，而不是替换现有 case。
