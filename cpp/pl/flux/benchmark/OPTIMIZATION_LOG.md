# Flux 优化记录

这份文档记录 `cpp/pl/flux` 在 2026-04-22 这一轮围绕 benchmark 做过的主要优化、验证方式和结果解读。目标不是替代 [README.md](/Volumes/workspace/liubang/playground/cpp/pl/flux/benchmark/README.md) 里的基线表，而是把“为什么会变快”以及“哪些数字该怎么理解”留下来，方便后续继续推进。

## 背景

最开始的切入点不是 benchmark，而是一轮面向功能矩阵和语义一致性的 review。那轮先修了几类 correctness 问题：

- `range()` 从字符串比较改成 RFC3339 时间比较，并修正为 stop-exclusive。
- 一元 `-` 支持 duration 字面量，比如 `range(start: -1h)`。
- `map`、`limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`、`reduce` 等算子恢复按逻辑表逐表执行，不再把多表流悄悄压扁。

把这些问题补稳后，benchmark 里最重的路径就更清楚了，尤其是：

- `aggregateWindow()`
- `group()`
- `pivot()`
- `join()`

## 优化顺序

这轮优化是按“先 correctness 保底，再做低风险结构性减法”的顺序推进的。

### 1. `aggregateWindow()`

最早的 `aggregateWindow()` 热点主要有三类：

- 先把所有行扁平处理，再靠字符串 bucket key 和 group key 回拼。
- 大量构造 `window_bucket_key` / `group_key` 字符串。
- 末尾对全量 bucket 做 `stable_sort`。

后来改成了更贴近真实语义的组织方式：

- 先按逻辑表处理。
- 在逻辑表内再按 group 维护窗口状态。
- tumbling window 场景，也就是 `every == period` 且 `period` 非负时，走单窗口快路径。

中间暴露出一个真实回归：如果同一个 chunk 里包含多个 `_group`，但我们只按 chunk 做聚合，就会把不同 group 合并掉。这个问题后来修成“按 chunk 内 group 分桶”，并加了专门回归测试锁住。

### 2. `keep/drop` 和 `pivot()`

这部分主要是把明显的 per-row 线性查找去掉：

- `keep()` / `drop()` 改成预建 `unordered_set`。
- `pivot()` 改成原地 upsert 属性，避免每插入一个 pivot 列都整行对象复制。
- `pivot()` 预建 `columnKey` / `rowKey` 集合，减少字段过滤时的重复判断。

### 3. `group()`

`group()` 原来对每一行会做两遍近似相同的工作：

- 先扫描一遍构 `_group` 对象。
- 再扫描一遍把同一批列转成字符串 key。

后面把它合并成一次扫描，同时产出 grouped row 和 group key，少掉了一轮列查找和一次对象构造。

### 4. `join()`

`join()` 这轮没有做激进重写，主要做的是小步减法：

- 预建 `on` 列集合。
- 减少重复 `contains`。
- 去掉 `join_rows()` 里重复做两遍的 group props 构造。

这类改动收益不一定像 `aggregateWindow()` 那样显著，但风险很低，适合持续累积。

## 2026-04-22 第二轮：`pivot/group/join` 收口

后面又补了一轮更贴着热点本身的优化，这轮重点不是继续扩能力，而是把已经暴露出来的三条热路径做得更扎实一些。

### 1. `pivot()` 按 logical table 局部建索引

之前的 `pivot()` 虽然已经做过一些减法，但 row identity 仍然是全表级别维护的。这会带来两个问题：

- 如果多个 logical table 里恰好有相同的 `rowKey`，实现上会发生跨 chunk 合并风险。
- 全表级 `row_indexes` 会随着输入规模持续膨胀，尤其在 `group() |> pivot(...)` 这类多表路径上不必要地放大字符串 key 的驻留时间。

后来改成了“每个 chunk 单独 pivot、单独建 identity map”。这样一来：

- 语义上更贴近 Flux 的逐 logical table 处理模型。
- 性能上也减少了 row identity map 的生命周期和峰值尺寸。

同时补了单测，专门锁住“两个不同 host 的 grouped table 即使 `rowKey` 一样，也不能被 pivot 合并”的回归。

## 2026-04-23：继续啃 `pivot_wide`

再往后做的这一刀更聚焦，目标就是把 `pivot_wide` 里最明显的两类额外开销压下去：

- 每写入一个 pivot 列值，都在线性扫描输出行属性，宽表下会越来越贵。
- 相同的 pivot 列名会在 chunk 内被反复构造，比如 `_field` 只有十几个取值，但会跨很多行重复出现。

这轮的处理方式比较直接：

- 每个输出行除了 `ObjectValue` 本体，再维护一份 `property_indexes`，这样更新已有 pivot 列时不再线性扫属性向量。
- pivot 列名改成 chunk 内缓存，相同 `columnKey` 组合只做一次列名构造。
- 新输出行在构造 base props 时，就把属性索引一起建起来，不再“先生成对象、再补扫一遍属性”。

这几处改动没有改变 pivot 的外部语义，但对宽表场景的对象布局和列写入路径更友好。

### 2. `group()` / `join()` 的 key 构造继续减法

这轮又补了几处很具体的减法：

- `group(mode: "except")` 不再对每个可见列线性 `contains`，而是先建 `excluded` 集合。
- `_group` key、row identity key、join key 不再先构造一份临时字符串再 append，而是直接往目标 buffer 里写。
- `join()` 的右侧 chunk 会先做一次 key 索引，避免同一个 right chunk 每和一个 left chunk 配对时都重扫一遍。

这些改动都不花哨，但对 `group/pivot/join` 这种“很多开销都死在字符串和重复扫描上”的路径很值。

## benchmark 侧的变化

为了让优化过程更可见，这轮也扩展了 benchmark 本身。

新增了两个 case：

- `group`：`csv.from |> group |> count |> yield`
- `pivot`：`csv.from |> pivot |> limit |> yield`

同时新增了专门的数据输入：

- `pivot_100000.annotated.csv`
- `pivot_500000.annotated.csv`
- `pivot_1000000.annotated.csv`

这样后续再优化 `group()` 或 `pivot()`，就不用只看 `agg` 这种混合路径。

再往后又补了更聚焦的热点 case：

- `pivot_wide`：更宽 `_field` 基数下的 pivot
- `join_grouped`：两侧先按 `host` 分表后再 join
- `agg_create_empty`：专门看空窗口扩张成本
- `agg_calendar`：专门看 calendar month 窗口路径

以及一组更适合本地迭代时快速 smoke 的 runner 参数：

- `--cases`
- `--metric-rows`
- `--join-rows`

## examples 校验

这轮不只是看 benchmark 和单点单测，也把 examples 一起纳入了回归面。

新增了一个 CLI 单测，会递归执行 `cpp/pl/flux/examples` 下所有 `.flux` example，确保：

- sample 不会和当前实现悄悄漂移。
- 做性能优化时，不会因为内部重排把 examples 跑挂。

这对 `aggregateWindow()` 那次“chunk 内不同 group 被错误合并”的回归尤其有帮助，因为它很快就在 example 路径上暴露出来了。

## 如何解读当前数字

当前 benchmark runner 更适合做“本地同机前后对比”，不适合当成严格冷启动基准。

原因有两点：

- 它直接拉起本地二进制，受文件缓存和系统热缓存影响很大。
- 连续多轮执行时，第二轮及之后的数字会明显偏低。

所以更靠谱的使用方式是：

1. 在同一台机器上，尽量用一致的顺序跑修改前。
2. 记录 JSON 输出。
3. 做改动。
4. 用同样的输入和顺序跑修改后。
5. 看相对变化，不要把单次绝对值当成最终结论。

如果后面要把这套东西用于更正式的性能门槛判断，建议补两类能力：

- 冷热分离。
- 输出更稳定的聚合统计，而不是只保留一次 `seconds`。

这部分现在已经向前走了一步：runner 默认会先做 warmup，再做多次采样，并输出 `samples_s`、`median_s`、`mean_s`、`min_s`、`max_s`。后续看性能结论时，应该优先看稳定采样后的中位数，而不是单次最好值。

在 `pivot_wide` 这条线上，最近这轮小口径 smoke 跑出来的数字是：

- `pivot:100000` 中位数约 `0.125s`
- `pivot_wide:100000` 中位数约 `0.167s`

这组结果明显比前一轮更低，但仍然应该按“同机同口径的本地比较信号”来理解，不要把它当成跨环境绝对承诺。

## 2026-04-23：从性能切回功能补齐

在把 `aggregateWindow()` 的几轮试探性优化回掉之后，这一轮没有继续盲目追 benchmark，而是先把能力面补齐了一批最常用、也最容易和现有执行器模型衔接的 builtin：

- `window()`
- outer `join`，也就是 `left` / `right` / `full`
- `spread()`
- `quantile(q:)`
- `median()`
- `top(n:)`
- `bottom(n:)`

这轮仍然同步扩了 benchmark case，但目的已经不是宣称“又提速了多少”，而是保证后续如果继续优化这些新路径时，有现成的 smoke 口径可以盯：

- `window`
- `ranking`
- `join_full`

这类 case 的价值主要在两点：

1. 新功能落地时，examples / 单测 / benchmark 能一起形成闭环。
2. 后面如果有人改 `window`、outer `join`、`top/bottom` 之类的新路径，不会只能靠体感判断有没有退化。

## 这一轮的结论

从代码层面看，这轮优化最有价值的不是某一个微调，而是建立了一个比较清晰的性能工作流：

- 先用 review 把语义风险和错误行为清掉。
- 再围绕最重路径做结构性减法。
- 同时让 benchmark 和 examples 都能反映改动是否值得、是否安全。

从工程层面看，这轮留下来的资产包括：

- 更完整的 benchmark case 集。
- 更稳定的多表流语义。
- 全量 example 执行兜底。
- 一份可以继续续写的优化记录。

## 下一步建议

- 给 benchmark 增加冷热分离或重复采样。
- 继续优化 `pivot()` 的 row identity / 列名构造，尽量减少字符串分配。
- 继续观察 `join()` 在更大 `on` 列集合和更宽 schema 下的行为，再决定是否值得做更大改造。

## 2026-05-16：connector page pipeline 继续补齐

这一轮围绕大表查询路径继续把执行主干做实：

- Top-N 从 single split 全局 SQL order 退化，推进为 split 内 partial Top-N + root pipeline 全局
  sort/limit。
- `PhysicalExecutor::ExecuteToSink` 补成真实 Page sink API，benchmark 可以不先物化完整
  `TableValue`。
- `sort/group/distinct/aggregate` 这类 blocking unary operator 直接收集 Page，`aggregate` 的
  count/sum/mean/min/max 已经消掉执行器内的 `TableValue` 聚合转换。
- `explain(physical: true)` 的 pipeline/profile 输出改成多行，并增加 drivers/split stats 信息，
  更容易看出 split 和 breaker pipeline。
- 新增 `mysql_scan_benchmark` 和 `run_connector_benchmarks.py`，用真实 MySQL 表或 SQLite
  生成表覆盖 scan/filter/topn 的 connector pipeline，并输出 repeat samples 的 median/mean。

这轮仍然不把单次 benchmark 数字写成跨环境性能承诺；新增 target 的价值是让后续每次改
connector split、page source 或 scheduler 时，都能用相同 DSN 和 scenario 复验行为与趋势。

## 2026-05-16：query scan 主干继续收口

这一轮只补查询主干，不提前进入 join/exchange 优化：

- `ExchangeOperator` 从整表 collect 边界改成逐 Page 的 gather/repartition，root exchange 不再把
  输入先物化成完整 `TableValue`。
- 内存侧 `filter/project` 的 Page 执行继续列式化，filter 预先解析列索引和 literal，避免每个
  predicate 都反复构造行对象。
- connector split profile 增加 `bytes` 和 `wall_ms`，profile formatter 与 SQLite/MySQL scan
  benchmark 都会输出 split bytes / split wall time。
- SQL connector pushdown 增加统一入口 contract，SQLite 参数化 SQL 和 MySQL dialect SQL 在生成
  SQL 前都先校验 projection、predicate、distinct、group/aggregate、sort、limit/offset 和 schema。

下一步仍然优先把高效 scan / pipeline 主干做厚；join 和更激进的 CBO alternative 等主干稳定后再动。

## 2026-05-16：SQLite / MySQL 同数据集 scan 对比

为了判断 MySQL connector 是否只是被小表固定开销拖慢，这轮构造了与 SQLite benchmark 同结构、
同生成逻辑、同查询形状的数据集：

- 表：`_time, host, region, usage, seq`。
- 生成逻辑：`host=edge-{seq % 64}`、`region=west/east`、`usage=seq % 100`。
- 查询：`filter_project`，也就是 `usage >= 50` 后投影 `host, usage`。
- MySQL 表名：`flux_bench_cpu`，`seq` 是 primary key，用于 range split。

结论：

- 同样数据和查询下，远程 MySQL 当前约比本地 SQLite 慢 `3.5x - 4.5x`。
- MySQL 路径仍然能稳定进入 range split + page sink 主干，输出行数和 split bytes 与 SQLite
  对齐，说明慢点主要在固定成本和 connector/page source 热路径，而不是 planner 走错。
- 下一步性能工作应优先看 MySQL 连接复用、metadata/statistics 缓存、split discovery 缓存、
  Boost.MySQL row decode 到 `ColumnVector` 的批量化，以及 page size / batch size 参数。

具体命令、表结构、采样数字和复现方式统一记录在
[benchmark/README.md](/Volumes/workspace/liubang/playground/cpp/pl/flux/benchmark/README.md)。

## 2026-05-16：MySQL scan 热路径 profile 与 fixture 自动化

这一轮把上次留下的 MySQL scan 性能工作继续做厚：

- MySQL connector metadata/statistics、primary key discovery、range split extent 增加进程内缓存，
  避免同一进程内重复 schema/count/min/max 固定开销。
- MySQL page source 的非 aggregate 路径直接解码到 `PageChunk` / `ColumnVector`，不再先构造整页
  `ObjectValue` rows。
- split profile 从 bytes/wall time 扩展为 metadata、split discovery、connect、schema、
  SQL build、execute、read、decode、page build 分段耗时。
- `sqlite_scan_benchmark` / `mysql_scan_benchmark` 输出同一组 profile 字段，方便横向看 SQLite
  本地文件扫描与远程 MySQL 协议扫描的差异。
- `run_connector_benchmarks.py` 支持 `--prepare-mysql-benchmark-table`，可以按 DSN 自动 drop/create
  同结构 MySQL 压测表，不再依赖手写 SQL fixture。

这轮的目标不是把 MySQL 和本地 SQLite 跑成同一个数字，而是让我们能定位慢在哪里。下一轮如果继续
优化 scan，优先看 direct streaming read batch、page size、协议 decode，以及更大规模数据下
split count 与远程服务吞吐之间的平衡。

## 2026-05-17：MySQL connection pool 与 scan 参数化

这一轮继续补 MySQL scan 主干的固定成本：

- 新增 MySQL runtime 级连接复用模块。后续验证发现自研 pool 和 Boost.MySQL 官方 pool 在
  pooled dynamic streaming read 下都会触发相同 ASAN container-overflow，因此自研 pool 已撤掉；
  当前只保留 Boost.MySQL 官方 `connection_pool` 的薄适配器，并限定在 metadata/statistics/split
  discovery 这类非 streaming 路径。
- MySQL runtime 参数化：`target_split_count`、`rows_per_page`、`max_pool_size`、
  `split_cache_max_entries`、`split_cache_ttl_ms` 都可以通过 benchmark runner 参数或环境变量控制。
- split extent cache 从无限 map 改为带 TTL/容量策略；TTL 为 `0` 时只按容量失效。
- benchmark runner 暴露 MySQL 调优参数，方便在 100k/1M/5M/10M 数据集上固定口径复验。
- prepared statement cache 暂不进入实现。当前 page source 已支持 server-side prepared scan，但
  streaming 使用独立直连；若后续重新评估 pooled streaming，必须先解决 Boost.MySQL dynamic
  `read_some_rows` 的 ASAN 问题。

这轮之后，MySQL scan 的下一层瓶颈应继续通过 profile 看：如果 `connect_ms` 降下来而 `read_ms` /
`execute_ms` 仍高，优先看 server/network/read batch；如果 `decode_ms` 抬头，再继续做 Value/string
allocation 优化。

本轮确认 benchmark：

- 1M `filter_project`：SQLite median `0.0620s`；MySQL remote samples `0.2068s / 0.2394s / 0.3377s`。
- 5M `filter_project`：SQLite `0.2712s`；MySQL remote `1.2079s`。
- MySQL 5M profile 中 `read_ms=6300.0`、`decode_ms=853.7`、`connect_ms=255.1`、
  `page_ms=1.64`。大表下 Page 构造已经不是主因，后续优化应更多看远程读取、协议 decode 和
  split discovery。

## 2026-05-17：MySQL prepared scan 与 split discovery 收口

这一轮把上次明确留下的 prepared statement 工作补完，继续只围绕高效 scan 主干：

- 通用 SQL builder 增加 `ParameterizedSql` 输出，where/time range/predicate/limit/offset 都生成
  placeholder + bind vector；literal SQL builder 仍保留给已有 SQLite 参数化实现和兼容入口。
- MySQL page source 默认走 server-side prepared statement，但每个 streaming page source 独立连接；
  自研 pool 已移除，Boost.MySQL 官方 `connection_pool` 只服务 metadata/statistics/split discovery。
  官方 pooled streaming connection 在 ASAN 下同样触发 Boost.MySQL dynamic row buffer 的
  container-overflow，已撤掉测试级 ASAN override，也不提供 pooled streaming 构造入口。
- MySQL metadata / split discovery 依靠进程内 schema/statistics/extent cache 和 Boost.MySQL 官方
  pool，避免控制面短查询污染 streaming page source 的连接状态。
- MySQL split discovery 不再为每次规划先查 `INFORMATION_SCHEMA.KEY_COLUMN_USAGE`；它优先使用
  schema 里的 `id` / `seq` / integer columns，再查 split extent。对 benchmark 表这类
  `seq primary key` 形态，少掉一次远程 metadata query。
- benchmark runner 增加 prepared on/off 参数，用同一张大表可以直接跑 literal SQL 与 prepared SQL
  的对比矩阵。

这轮之后，MySQL scan 主干已经具备：split extent cache、parameterized SQL、server-side prepared
execution、page source streaming、profile 分段耗时。连接池和 prepared statement cache 不再作为已
接入能力记录；后续要重新启用，必须先用更小复现或 owning/static read path 证明 streaming read
生命周期安全。

本轮确认 benchmark：

- 5M `filter_project`，8 splits，rows/page=2048，prepared on：samples
  `1.1746s / 1.4447s / 1.2069s`，median `1.2069s`，输出 `2.5M` rows。
- 同表同查询 prepared off：samples `1.2397s / 1.2799s / 1.4307s`，median `1.2799s`。
- 结论：prepared on 在这组远程 MySQL 测试里略好，但总耗时仍主要由远程 `read_ms` 和 decode 主导；
  prepared 不是主要瓶颈，下一步更值得看 read batch / protocol decode / Value allocation。

## 2026-05-17：并行执行取消传播收口

这一轮没有继续堆新算子，而是把高效 scan 主干的失败路径补干净：

- `Operator` 增加 `Cancel()`，`OutputOperator`、unary operator、`ExchangeOperator`、
  `ExchangeSourceOperator`、`ExchangeSinkOperator` 和 `LocalHashJoinOperator` 都会把 cancel
  递归传给上游。
- root sink 或 consumer 侧出错时，相关 `ExchangeBuffer` 会被关闭，producer 即使正在 page/byte
  budget 背压上等待，也能被唤醒并退出。
- `Scheduler` 等待所有 driver task 收尾后，优先返回 root/output 侧原始错误，避免 producer 的
  `buffer closed` 覆盖真正的查询错误。
- 新增回归测试：6000 个 1-row page、4 splits、root sink 第一页返回 cancel，验证 exchange
  producer 不会因为 buffer 填满而挂住。

本轮复验：

- `bazel test //cpp/pl/flux/runtime:runtime_exec_unit_test`：97 tests，96 passed，1 skipped
  MySQL integration。
- 真实 SQLite benchmark：
  - 1M `scan`：8 drivers，100 万 output rows，984 pages，`1.1252s`，约 `888,715` input rows/s。
  - 1M `filter_project`：8 drivers，50 万 output rows，496 pages，`0.2749s`，约 `3.64M`
    input rows/s，非 blocking。
