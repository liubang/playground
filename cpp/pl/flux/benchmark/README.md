# Flux 基准测试

这个目录保存 Flux 运行时和 connector scan 的本地基准测试。目标不是做完全隔离的微基准，
而是给我们一个可重复的比较方法，用来观察引擎优化前后的变化。

如果想看这轮优化是怎么一步步做出来的，可以配合阅读 [OPTIMIZATION_LOG.md](/Volumes/workspace/liubang/playground/cpp/pl/flux/benchmark/OPTIMIZATION_LOG.md)。

## Benchmark 层次

当前基准分三类：

- 内存执行基准：`run_benchmarks.py` 生成 annotated CSV，覆盖 table builtin、window、pivot、
  join 等内存执行路径。
- SQLite connector scan：`sqlite_scan_benchmark` 临时构造真实 SQLite 表，覆盖 multi-split
  page source、Page sink、Top-N 等 query pipeline。
- MySQL connector scan：`mysql_scan_benchmark` 读取真实 MySQL 表，覆盖 range split、
  Boost.MySQL page source、远程服务和协议解码成本。

数字默认用于同机同口径前后对比；跨机器、冷/热缓存、远程数据库网络条件不同，不能直接比较。

## 覆盖内容

当前基准集覆盖这些有代表性的查询形态：

- `linear`：`csv.from |> filter |> map |> limit`
- `sort`：`csv.from |> sort |> limit`
- `agg`：`csv.from |> group |> aggregateWindow |> yield`
- `agg_create_empty`：`aggregateWindow(createEmpty: true)`，专门观察空窗口扩张成本
- `agg_calendar`：`aggregateWindow(every: 1mo)`，专门观察 calendar window 路径
- `group`：`csv.from |> group |> count |> yield`
- `window`：`csv.from |> range |> group |> window(createEmpty) |> count`
- `array`：`findColumn + array.map/filter/contains/reduce/any/all`
- `ranking`：`csv.from |> top / bottom`
- `pivot`：`csv.from |> pivot |> limit`
- `pivot_wide`：更宽的 `_field` 基数下直接 `pivot`
- `join`：双表 `join(..., on: ["_time", "host"]) |> limit`
- `join_grouped`：两侧先 `group(columns: ["host"])` 再 `join(..., on: ["_time"])`
- `join_full`：双表 `join(method: "full") |> limit`
- `sqlite_scan_benchmark`：构造真实 SQLite 表，走 connector multi-split `filter + project`

这些 case 组合起来主要覆盖：

- 近似线性的轻管道
- 全表重排
- 带状态的聚合路径
- 带空窗口扩张的聚合路径
- calendar month 窗口路径
- 纯 group 重分表路径
- 显式窗口拆表与空窗口保留路径
- 数组 package 的真实 helper 组合路径
- 排名 selector 路径
- 宽表整形路径
- 更重的多表 `join` 路径，目前已经由哈希索引支撑
- outer join 路径
- `pivot` / `join` 在多 logical table 语义下的热点路径
- SQLite connector 的真实多 split streaming scan 路径
- connector split profile 的 bytes / wall time 和 metadata/split/connect/schema/sql/execute/read/decode/
  page-build 分段指标

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

runner 会输出 JSON，默认按“`1` 次 warmup + `5` 次采样”执行每个 case，并给出样本和聚合统计，便于在修改前后直接对比：

```json
[
  {
    "case": "linear:100000",
    "rc": 0,
    "warmup_runs": 1,
    "repeat_runs": 5,
    "samples_s": [0.08, 0.081, 0.079, 0.08, 0.081],
    "median_s": 0.08,
    "mean_s": 0.0802
  }
]
```

如果你只想快速本地 smoke 一轮，也可以显式调小：

```bash
python3 cpp/pl/flux/benchmark/run_benchmarks.py --warmup-runs 0 --repeat-runs 3
```

如果你只想盯某几个热点，也可以只跑指定 case 和输入规模：

```bash
python3 cpp/pl/flux/benchmark/run_benchmarks.py \
  --warmup-runs 0 \
  --repeat-runs 2 \
  --cases agg,agg_create_empty,agg_calendar,window,ranking,pivot,pivot_wide,join,join_grouped,join_full \
  --metric-rows 100000 \
  --join-rows 2000
```

SQLite connector 有独立的真实 scan benchmark target。它会在 `/tmp` 构造 SQLite 数据库，
插入指定行数，执行真实 connector query，并输出 scenario、drivers、pages、split bytes、
split wall time、blocking 和吞吐。
默认 scenario 是 `filter_project`，也可以显式跑 `scan`、`wide_filter`、`topn`、
`group_count`、`group_sum`、`group_mean`、`distinct_host`。其中 group/distinct 场景会用
materialize barrier 固定走本地 Page-native accumulator，避免被 SQLite 聚合下推掩盖：

```bash
bazel build //cpp/pl/flux/benchmark:sqlite_scan_benchmark
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_scan.db scan
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_wide.db wide_filter 80
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_topn.db topn
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_group.db group_count
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_group_sum.db group_sum
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_group_mean.db group_mean
bazel-bin/cpp/pl/flux/benchmark/sqlite_scan_benchmark 1000000 /tmp/flux_distinct.db distinct_host
```

2026-05-17 复验的本机真实 SQLite 结果：

| scenario | rows | drivers | output rows | pages | blocking | seconds | input rows/s |
| --- | ---: | ---: | ---: | ---: | :---: | ---: | ---: |
| `scan` | 1,000,000 | 8 | 1,000,000 | 984 | false | 1.1252 | 888,715 |
| `filter_project` | 1,000,000 | 8 | 500,000 | 496 | false | 0.0577 | 17,316,700 |
| `distinct_host` | 1,000,000 | 8 | 64 | 985 | true | 0.0867 | 11,540,100 |
| `group_count` | 1,000,000 | 8 | 64 | 985 | true | 0.3851 | 2,596,790 |
| `group_sum` | 1,000,000 | 8 | 64 | 985 | true | 0.3820 | 2,617,550 |
| `group_mean` | 1,000,000 | 8 | 64 | 985 | true | 0.3947 | 2,533,510 |

`group_count` 当前刻意绕开 SQLite SQL aggregate pushdown，用来压本地 accumulator 主线。本轮把
`group |> aggregate` 融合成直接从输入 Page 更新 aggregate state 的执行路径后，1M
`group_count` 从上一版 `23.8288s` 降到顺序复跑的 `0.3851s`。剩余成本主要集中在 group key 构造、哈希和
SQLite page 读入，不再是整表 row-object 中间态。

MySQL connector 也有独立 scan benchmark target，用真实 MySQL 表验证 range split、
streaming page sink 和两阶段 Top-N。它默认读取 `FLUX_MYSQL_TEST_DSN`，表名默认 `cpu`，
scenario 默认 `filter_project`，也可以显式跑 `scan`、`wide_filter`、`topn`。如果参数里的
dsn 传空字符串，benchmark 会回退读取 `FLUX_MYSQL_TEST_DSN`，便于只替换表名：

```bash
bazel build //cpp/pl/flux/benchmark:mysql_scan_benchmark
FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  bazel-bin/cpp/pl/flux/benchmark/mysql_scan_benchmark
bazel-bin/cpp/pl/flux/benchmark/mysql_scan_benchmark \
  'mysql://flux:flux@192.168.50.31:3306/testdb' cpu topn
FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  bazel-bin/cpp/pl/flux/benchmark/mysql_scan_benchmark '' flux_bench_cpu filter_project 50
```

如果要做同机多轮对比，用 connector benchmark runner 统一跑 repeat samples。它会输出
每个 scenario 的 `samples_s`、`median_s`、`mean_s`、`min_s`、`max_s`，并保留最后一轮的
drivers/output rows 和 split profile 分段耗时：

```bash
bazel build //cpp/pl/flux/benchmark:sqlite_scan_benchmark \
  //cpp/pl/flux/benchmark:mysql_scan_benchmark
python3 cpp/pl/flux/benchmark/run_connector_benchmarks.py \
  --connector sqlite --sqlite-rows 1000000 --repeat 3
FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  python3 cpp/pl/flux/benchmark/run_connector_benchmarks.py \
    --connector mysql \
    --mysql-target-splits 8 \
    --mysql-rows-per-page 1024 \
    --mysql-max-pool-size 8 \
    --repeat 3
```

MySQL runtime 可调参数：

- `--mysql-target-splits` / `FLUX_MYSQL_TARGET_SPLITS`：可拆分 scan 的目标 split 数。
- `--mysql-rows-per-page` / `FLUX_MYSQL_ROWS_PER_PAGE`：page source 每页目标行数。
- `--mysql-max-pool-size` / `FLUX_MYSQL_MAX_POOL_SIZE`：Boost.MySQL 官方
  `connection_pool` 的最大连接数。当前用于 metadata/statistics/split discovery；
  streaming page source 固定使用独立直连，因为官方 `pooled_connection` 搭配 dynamic
  `read_some_rows` 在 ASAN 下仍会触发 Boost.MySQL 内部 container-overflow。
- `--mysql-split-cache-max-entries` / `FLUX_MYSQL_SPLIT_CACHE_MAX_ENTRIES`：split extent 缓存容量。
- `--mysql-split-cache-ttl-ms` / `FLUX_MYSQL_SPLIT_CACHE_TTL_MS`：split extent 缓存 TTL；设为 `0`
  表示不按时间过期。
- `--mysql-disable-prepared-statements` / `FLUX_MYSQL_USE_PREPARED_STATEMENTS=0`：关闭
  MySQL page source 的 server-side prepared statement 路径，用于和 literal SQL 回退路径对比。

MySQL benchmark fixture 可以由 runner 自动重建。它会用和 SQLite benchmark 相同的数据形态创建
表，并把 `seq` 设为 primary key，便于 MySQL connector 做 range split。这个步骤会先 drop 目标表：

```bash
FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  python3 cpp/pl/flux/benchmark/run_connector_benchmarks.py \
    --connector mysql \
    --mysql-table flux_bench_cpu \
    --mysql-rows 1000000 \
    --mysql-target-splits 8 \
    --mysql-rows-per-page 1024 \
    --mysql-max-pool-size 8 \
    --prepare-mysql-benchmark-table \
    --scenario filter_project \
    --repeat 3
```

prepared statement / page size 矩阵建议保持同一张 `flux_bench_cpu` 表反复跑，避免把建表成本混进
scan 数字：

```bash
FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  python3 cpp/pl/flux/benchmark/run_connector_benchmarks.py \
    --connector mysql \
    --mysql-table flux_bench_cpu \
    --mysql-target-splits 8 \
    --mysql-rows-per-page 2048 \
    --mysql-max-pool-size 8 \
    --scenario filter_project \
    --repeat 5

FLUX_MYSQL_TEST_DSN='mysql://flux:flux@192.168.50.31:3306/testdb' \
  python3 cpp/pl/flux/benchmark/run_connector_benchmarks.py \
    --connector mysql \
    --mysql-table flux_bench_cpu \
    --mysql-target-splits 8 \
    --mysql-rows-per-page 2048 \
    --mysql-max-pool-size 8 \
    --mysql-disable-prepared-statements \
    --scenario filter_project \
    --repeat 5
```

当前记录的一组 5M 行 `filter_project` 结果：

- prepared on，8 splits，rows/page=2048：`1.1746s / 1.4447s / 1.2069s`，median `1.2069s`。
- prepared off，同表同查询：`1.2397s / 1.2799s / 1.4307s`，median `1.2799s`。

这说明 prepared 默认开启有收益，但远程 MySQL 大表 scan 的主成本仍在 `split_read_time_ms` 和
`split_decode_time_ms`。

profile 字段含义：

- `split_metadata_time_ms`：split manager 读取 schema/capability/statistics 等 metadata 的耗时。
- `split_discovery_time_ms`：split manager 生成 split 列表的总耗时。
- `split_connect_time_ms`：page source 建立 MySQL 连接的耗时。
  连接池命中时这个值接近 acquire 成本，创建新连接时包含真实 connect 成本。
- `split_schema_time_ms`：page source 初始化时读取 schema 的耗时；命中进程内 cache 时应很低。
- `split_sql_build_time_ms`：把 `ScanRequest` 编译成 SQL 的耗时。
- `split_execute_time_ms`：MySQL start execution 的耗时。
- `split_read_time_ms`：从 MySQL 协议读取 rows batch 的累计耗时。
- `split_decode_time_ms`：Boost.MySQL field 到 Flux `Value` / `ColumnVector` 的累计耗时。
- `split_page_build_time_ms`：把解码结果封装成 `Page` 的累计耗时。

## 数据形态

生成的基准输入默认位于 `/tmp/flux_bench`：

- `metrics_100000.annotated.csv`
- `metrics_500000.annotated.csv`
- `metrics_1000000.annotated.csv`
- `pivot_100000.annotated.csv`
- `pivot_500000.annotated.csv`
- `pivot_1000000.annotated.csv`
- `pivot_wide_100000.annotated.csv`
- `pivot_wide_500000.annotated.csv`
- `pivot_wide_1000000.annotated.csv`
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

### Connector Scan：SQLite vs MySQL 同数据集

2026-05-16 用同结构、同数据生成逻辑、同查询形状做了一轮 SQLite/MySQL 对比。

表结构：

```text
_time  string/varchar
host   string/varchar
region string/varchar
usage  double
seq    integer/bigint
```

数据生成逻辑：

- `_time = 2024-07-01T10:{seq % 60}:00Z`
- `host = edge-{seq % 64}`
- `region = west/east`
- `usage = seq % 100`
- `seq` 为单调整数；MySQL 表上 `seq` 是 primary key，方便 range split。

查询形状：

```text
scan |> filter(usage >= 50) |> project(host, usage)
```

100k 行结果：

| Connector | 输入行 | 输出行 | Drivers | Pages | Split bytes | Split wall time | 端到端时间 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| SQLite | 100k | 50k | 8 | 56 | 6.38 MB | 39.2 ms | 0.0088s median |
| MySQL remote | 100k | 50k | 8 | 53-55 | 10.39-10.80 MB | 182.8 ms | 0.0693s median |

1M 行结果：

| Connector | 输入行 | 输出行 | Drivers | Pages | Split bytes | Split wall time | 端到端时间 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| SQLite | 1M | 500k | 8 | 496 | 63.61 MB | 339.8 / 341.4 / 350.4 ms | 0.0618 / 0.0620 / 0.0658s |
| MySQL remote | 1M | 500k | 8 | 487-488 | 101.98-102.80 MB | 977.3 / 1281.8 / 1695.2 ms | 0.2068 / 0.2394 / 0.3377s |

5M 行单次确认结果：

| Connector | 输入行 | 输出行 | Drivers | Pages | Split bytes | Split wall time | 端到端时间 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| SQLite | 5M | 2.5M | 8 | 2448 | 317.99 MB | 1423.7 ms | 0.2712s |
| MySQL remote | 5M | 2.5M | 8 | 2412 | 511.71 MB | 7284.6 ms | 1.2079s |

MySQL 5M profile：`split_read_time_ms=6300.0`，`split_decode_time_ms=853.7`，
`split_connect_time_ms=255.1`，`split_execute_time_ms=216.2`，`split_page_build_time_ms=1.64`。
这说明大表下主瓶颈已经清晰集中到远程 read 和协议 decode，Page build 仍不是热点。

解读：

- 同样 `filter_project` 查询下，远程 MySQL 端到端仍明显慢于本地 SQLite；本轮优化后小表固定开销
  下降明显，但远程读耗时抖动会拉大 1M 行尾部延迟。
- MySQL 慢的主要来源不是输出行数不同，而是远程服务、网络、range split discovery、
  MySQL execution/read 和协议解码成本；Page build 已经低到毫秒级以下或接近毫秒级。
- Boost.MySQL 官方 connection pool 已接入 runtime 控制面，但这个 benchmark 每个 sample 是独立进程；
  pool 主要降低 metadata/statistics/split discovery 的固定成本。streaming page source 仍是独立直连，
  因此它不会把首个大并发 scan 的 connect 成本清零。
- SQLite 是本机文件，且 benchmark 进程内创建数据库后立即查询，缓存条件更有利。
- 这组数据说明 MySQL connector 主干能稳定 multi-split 扫描 1M 行级别数据，但后续优化应优先
  盯连接复用、split discovery 固定开销、read batch/page size 参数，以及远程服务吞吐稳定性。

### Legacy In-Memory Baseline

下面这组基线会随着运行时实现变化持续更新。当前这版是在 2026-04-22 本地采集的，包含：

- 基于哈希索引的 `join`
- `aggregateWindow` 的按逻辑表/按 group 聚合路径，以及 tumbling window 快路径
- `aggregateWindow(createEmpty: true)` 的空窗口扩张路径
- `aggregateWindow(every: 1mo)` 的 calendar window 路径
- `group()` 的单次扫描分组 key / `_group` 构造路径
- `pivot()` / `join()` 中几处字符串 key 构造与集合判断的减法
- `pivot()` 按 logical table 局部建索引，避免跨 chunk 合并和全表 row identity map 膨胀
- `join()` 右侧 chunk 预建 key 索引，避免同一个 right chunk 被重复扫多遍
- selection / reordering 算子中的行指针复用
- `array.contains` / `array.reduce` / `array.any` / `array.all` 所在的数组 helper 路径

注意：这套 runner 更适合做“本地同机前后对比”，并不追求冷启动隔离；如果你连续运行多轮，结果仍会受热缓存影响，但现在默认会用多次采样的 `median` / `mean` 帮助降低单次抖动带来的误判。

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

这轮为了专门观察 `pivot` / `join` 热点，还额外做了一次小口径 smoke：

| Case | 输入规模 | 中位数 |
| --- | --- | ---: |
| `pivot` | 100k rows | 0.125s |
| `pivot_wide` | 100k rows | 0.167s |
| `join` | 2000 x 2000 rows | 0.010s |
| `join_grouped` | 2000 x 2000 rows | 0.013s |

这组数字主要用来证明新 case 和 runner 口径都正常，不直接替换上面的完整基线表。

## 如何使用

当我们修改运行时行为时，推荐流程如下：

1. 在当前分支先跑一遍基准。
2. 保存 `run_benchmarks.py` 的 JSON 输出。
3. 修改运行时实现。
4. 用同样的输入规模再跑一遍。
5. 优先对比 `median_s`，再参考 `mean_s` 和 `samples_s` 的离散程度。
6. 只有当同口径重复采样后仍然稳定改善，才把结果写进文档或优化记录。

如果后续新增更重的算子、更多 join 形态，或者真正的流式 reader，建议在这里新增新的 benchmark case，而不是替换现有 case。
