# Flux Data Source Architecture Plan

本文档记录 `cpp/pl/flux` 的数据源、optimizer 和 physical execution 架构。早期目标是先支持
SQLite/MySQL 这类外部表数据源，再逐步演进到带查询计划、算子下推和内存回退的执行架构；
截至 2026-05-16，这条主干已经进入 `ConnectorRuntime -> Split -> PageSource -> Pipeline ->
Driver -> Operator -> Page` 的单机查询执行形态。

当前运行时不是纯 eager interpreter：`array.from` / `csv.from` / `sqlite.from` / `mysql.from`
仍对外表现为 Flux table stream，但数据源入口会携带 lazy logical plan，输出或 fallback
边界再由 physical executor 决定是否走 connector pushdown、page streaming 或 `TableValue`
materialization。旧内存执行器没有被推倒重写，而是被收束为无法下推算子的可靠 fallback。

## Long-Term Shape

这个架构的长期目标可以定义为：

```text
Flux-native single-node federated query engine
```

也就是：以 Flux 为统一查询接口的单机联邦查询引擎。

它会借鉴 Presto/Trino 的真实执行流程，而不只是借用术语或 explain 外观：

- 统一语言入口。
- analyzer / binder 把语言层 AST 转成类型化语义对象。
- logical plan 作为执行的一等输入，而不是 `TableValue` 的调试 metadata。
- rule-based optimizer 负责确定性 rewrite 和 connector pushdown。
- cost-based optimizer 框架负责 statistics、cost 和 alternative plan 的选择；缺统计时明确退化为 RBO。
- physical planner 把 optimized logical plan 编译成 pipeline/driver/operator 形态。
- connector abstraction 拆成 metadata、split manager、page source provider。
- scheduler 保留 task/split/driver 边界；当前是单机 worker pool，不做跨节点调度。
- operator 以 page/chunk 流为主通道，`TableValue` 只作为输出或 fallback materialization。

但它不是完整 Presto，也不以第一阶段分布式执行为目标。暂不考虑：

- coordinator / worker 架构。
- distributed exchange。
- shuffle。
- cross-node fault tolerance。
- 分布式 resource group / cluster management。

需要注意：虽然第一阶段不做多节点，单机执行也要保留 split、task、driver、operator、
exchange 边界的简化形态。这样后续加并发、本地 exchange、跨源 join 或更多 connector 时，
不会推翻前面的执行主干。

与 Presto 的另一个重要区别是，对外接口不是 SQL federation，而是 Flux federation。
SQL 数据源可以通过 SQL dialect 下推执行，CSV、内存表、后续 Parquet/HTTP 等数据源则
通过各自 connector 的 native scan 能力进入同一个 Flux logical plan。查询语义以 Flux
的 table stream、group key、pipe、time range、window 等模型为中心，而不是把所有东西
先压成 SQL relational model。

因此后续设计可以放心采用 Presto-like 的 connector / planner / pushdown 分层，但不要把
coordinator、worker、exchange、cost-based distributed optimization 这类分布式系统复杂度
带进第一阶段架构。

## Goals

- 支持多类数据源：内联内存 rows、CSV、SQLite、MySQL，后续可扩 PostgreSQL、HTTP、Parquet 等。
- 保留当前 Flux 子集的用户可观察行为，特别是多逻辑表流、group key、时间范围和输出格式语义。
- 为数据源下推建立清晰边界，让 `range/filter/keep/limit/sort` 等简单算子能靠近数据执行。
- 对不能安全下推的 Flux 语义，自动 materialize 到 `TableValue` 后复用现有内存 builtin。
- 让第一阶段实现足够小，不为尚未出现的分布式执行、复杂 cost model、join reorder 过早付费。

## Current Status Snapshot

当前事实以本节为准；后面的 Phase 记录保留演进历史和设计动机。

- 用户数据源入口：`array.from`、`csv.from`、`sqlite.from`、`mysql.from`。没有 universe 顶层
  `from(bucket:)`。
- Connector runtime：SQLite、MySQL 和 memory runtime 都走 metadata / split manager /
  page source provider；registry、optimizer、physical execution 不再依赖旧 scan factory。
- MySQL runtime：metadata、statistics 和 split discovery 使用 Boost.MySQL 官方
  `connection_pool`；streaming page source 使用独立直连，避免 pooled dynamic
  `read_some_rows` 在 ASAN 下触发 Boost.MySQL 内部 container-overflow。rows/page、split count、
  pool max size、split cache TTL/容量均可配置。
- Pushdown：SQLite/MySQL 支持保守 `range/filter(simple)/keep/drop/rename/sort/limit/distinct`
  线性前缀，以及简单 `group(columns:) |> count/sum/mean/min/max(column:)` 聚合。SQL 生成前有统一
  pushdown contract 校验。
- Split：SQLite 可按 `rowid` multi-split；MySQL 可按主键或整型列 range split；带全局
  order/offset/aggregate/distinct/group 语义的请求会回退 single split，避免跨 split 语义漂移。
- Execution：physical planner 产出 `ExecutionTask` / `Pipeline`，scheduler 通过 `TaskExecutor`
  运行 driver task；operator 之间主通道是 `Page` / `PageChunk` / `ColumnVector`。root 输出错误会
  触发 operator cancel，关闭上游 exchange buffer，避免 producer 在背压队列上挂住。
- Streaming：scan/filter/project/range 和 root exchange 已经逐 Page 执行；group/distinct/
  aggregate 是 streaming accumulator，逐 Page 吸收输入并最终产出结果；多 driver 的
  `group |> aggregate` 会拆成 driver-local partial 和 global final 两阶段，final 只合并 partial
  结果；sort/topn/join/materialize 仍是明确 blocking boundary。
- Profile：pipeline profile 包含 drivers/pages/rows/blocking/finished/error；connector split
  profile 包含 pages/rows/bytes/wall time，以及 metadata/split/connect/schema/sql/execute/read/
  decode/page-build 分段耗时；formatter 会标出 blocking pipeline 内的 accumulator 算子，并输出
  accumulator phase/key strategy/partial-final 耗时。
- Benchmark：已有 SQLite/MySQL scan benchmark target，可用同结构数据集比较本地 SQLite 和远程
  MySQL connector scan 路径；runner 可自动重建 MySQL benchmark 表，也可一条命令重建 release
  benchmark binary 并写 JSON baseline。最新 SQLite grouped accumulator 复验见
  [benchmark/README.md](./benchmark/README.md)。

## Non-Goals

- 第一阶段不实现 Presto/Trino 级别的完整分布式查询引擎。
- 第一阶段不要求 Flux 任意函数表达式都能翻译成 SQL。
- 第一阶段不做跨数据源 join 下推、复杂窗口下推或完整 cost-based optimizer；当前 CBO 只作为
  statistics/cost/alternative framework，缺统计时明确退化为 RBO。
- 第一阶段不改变 parser 语法，只新增 package/builtin 和运行时执行结构。

## Current Architecture

历史上主要数据流如下：

```text
Flux source
  -> parser AST
  -> ExpressionEvaluator / StatementExecutor
  -> builtin function callback
  -> TableValue rows/tables
  -> next builtin callback
  -> output formatter
```

现在仍保留这些用户入口，但 SQL provider 的输出边界已由 physical executor 接管：

- `array.from(rows:, bucket:)`：从对象数组构造内联内存表。
- `csv.from(csv:, file:, mode:)`：解析 raw/annotated CSV 并构造表。
- `sqlite.from(path:, table:)`：扫描 SQLite 表；用户 API 不提供 raw SQL/query 入口。
- `mysql.from(dsn:, table:)` / `mysql.from(host:, user:, password:, database:, table:, ?port:)`：通过 Boost.MySQL 扫描 MySQL 表；用户 API 不提供 raw SQL/query 入口。

运行时不提供 universe 顶层 `from(bucket:)` 或其他顶层数据源占位。新增数据源必须走 provider package 入口，避免让旧的顶层 `from` 写法重新长回来。

表变换入口：

- `range/filter/map/keep/drop/rename/limit/tail/sort/group/...` 都接收 `tables`
  pipe 参数。
- 大多数逐行变换使用 `transform_rows_preserving_chunks` 遍历 `TableValue.tables`。
- `TableValue.rows` 是 flatten 后的便捷视图，`TableValue.tables` 承载逻辑表流语义。

这个 eager 结构的优点是简单、可调试、测试覆盖集中；它现在主要作为 legacy builtin fallback
和最终输出 materialization 的实现基础，不再是 SQL connector 扫描的主路径。

## Target Architecture

目标结构按真实查询引擎流程拆成以下层次：

```text
Flux AST
  -> Analyzer / Binder
  -> LogicalPlan
  -> RBO
  -> CBO framework
  -> PhysicalPlan
  -> Scheduler
  -> Driver / Operator pipeline
  -> Page / Chunk stream
  -> TableValue materialization
```

核心思想：

- 数据源 builtin 不立即全量读取，而是尽量返回一个可延迟执行的表计划。
- 表变换 builtin 优先把自己追加为 logical operator。
- builtin 不负责 optimizer 决策，只负责把语言级调用翻译成 logical node。
- RBO 负责 connector 能处理的前缀下推、projection pruning、barrier insertion。
- CBO 只在有 statistics 时选择 alternative plan；缺信息时保持 RBO 输出，不伪造精度。
- physical planner 明确产出 connector scan、memory operator、materialize、output sink。
- scheduler 把 split 分配给 driver；当前单机执行允许一个 pipeline 展开多个 driver root。
- connector 能处理的前缀在 source 执行；不能处理的后缀通过 memory operator 执行。

截至目前，connector、logical plan skeleton、SQLite/MySQL 保守下推和简单聚合下推已经落地。
物理执行已经进入 `ExecutionTask -> Pipeline -> Scheduler -> Driver -> Operator -> Page`
主干：connector scan 走 metadata / split manager / page source provider，scan/filter/project/range
可以保持 page streaming，多 split 会展开为多个 driver，本地 exchange 会把 producer pipeline
接到 root pipeline。全局 Top-N 已经使用两阶段执行：split 内先做 partial Top-N，root pipeline
再做全局 heap Top-N。`group/distinct/aggregate` 是 Page-native streaming accumulator：输入逐页
吸收进 group/distinct/aggregate state，最终产出结果页，不再先把整个输入拼成 `TableValue` 作为跨层
主接口。`group |> aggregate` 会进一步融合成 grouped aggregate accumulator，逻辑/profile 仍保留
`GroupOperator` 与 `AggregateOperator`，物理执行直接从输入 Page 更新 aggregate state。
`sort/topn/join/materialize` 仍是明确 blocking boundary。profile 会汇总 connector split 的
pages/rows/bytes/time/finished 状态，physical explain/profile 输出按多行展示 pipeline 的 drivers、
依赖、blocking、accumulators、operators 和 split stats。

接下来优先级不再是继续扩展更多数据源，而是继续把高吞吐查询主干做厚：减少 SQL connector
固定开销、缓存 metadata/statistics、优化 MySQL page source 转换路径、完善 streaming
blocking boundary 的 profile，再考虑更激进的 CBO alternatives。

## Presto-Like Execution Contract

后续每个实现步骤都需要遵守以下边界，避免短期功能导致长期返工：

- Analyzer / Binder：只处理语言层语义、参数形态、函数绑定和类型信息，不访问外部数据。
- LogicalPlan：表达用户查询语义和 Flux table stream 边界，不包含具体 SQL 字符串或 connector
  执行对象。
- RBO：所有确定性 rewrite 都在这里，包括 predicate/projection/limit/sort/aggregate pushdown、
  column pruning、barrier insertion 和不支持 lazy 的 fallback 标记。
- CBO：提供 statistics/cost/alternative plan 接口。第一版允许所有 cost 为 unknown，但
  physical plan 必须能记录“为什么没有做 cost-based 选择”。
- PhysicalPlan：只描述执行形态，不直接执行；包含 table scan、filter、project、aggregation、
  join、exchange、materialize、output 等 physical node。
- Scheduler：把 physical plan 转为 task/pipeline/driver。当前单机版通过 worker pool 运行
  driver task，API 不能假设只有一个 driver。
- Operator：消费和产生 page/chunk。row-by-row 可以作为内部实现细节，但不能成为长期跨层接口。
- Connector：拆成 metadata、split manager、page source provider。registry、optimizer 和 physical
  execution 只面向 connector runtime；不再存在共享 `TableSource` 执行抽象。
- Materialization：只有输出、inspect、旧 builtin fallback、跨源 join 等边界才把 page/chunk
  materialize 成 `TableValue`。

任何新数据源都必须走 connector 边界；任何新算子都必须先进入 logical/physical 计划，再决定
是否提供 eager fallback。除非是在迁移旧代码，否则不再把优化逻辑写进 runtime builtin。

## Code Organization Style

后续实现需要刻意区分“数据结构”和“可插拔架构边界”，不要在两边使用同一种代码风格。

适合保持轻量 struct / free function 的部分：

- `Value` / `ObjectValue` / `TableValue` 这类运行时值模型。
- logical / physical plan node 这类 IR 数据结构。
- 谓词、projection、scan request、cost estimate、statistics estimate 等小型不可变/半不可变数据。
- 格式化、字面量转换、简单校验、列名映射等无状态 helper。

这些部分应优先保持透明、易复制、易测试，避免为每个节点或字段引入不必要的 virtual class。

必须使用 interface / class 边界的部分：

- Connector：`ConnectorMetadata`、`ConnectorSplitManager`、`ConnectorPageSourceProvider`。
- Optimizer：`PlanOptimizer`、`Rule`、`RuleSet`、`StatsProvider`、`CostCalculator`。
- Physical planning：`PhysicalPlanner`、operator factory、pipeline builder。
- Execution：`Scheduler`、`Task`、`Pipeline`、`Driver`、`Operator`、`PageSource`、`OutputBuffer`。
- Runtime service：执行上下文、内存/错误/统计收集、trace/explain collector。

这些部分需要通过 OOP 表达稳定生命周期、可替换实现和多态扩展点。未来新增数据源、新增 rule、
新增 physical operator 时，应优先新增实现类并注册到对应边界，而不是修改 builtin 或在
planner 中堆条件分支。

总体约束：

- IR 用数据结构，执行边界用接口。
- builtin 只做语言层参数解析和 logical node 构造；不承载 optimizer、connector 或 executor 决策。
- optimizer rule 以独立对象/接口组织，支持 rule trace 和单测，不写成散落在 builtin 里的 helper。
- connector 不暴露 SQL 字符串构造细节给 planner；planner 只处理 handle、constraint、assignment。
- operator 之间传递 page/chunk，不把 `TableValue` 当作长期跨层数据通道。
- connector 内部可以保留具体 source helper 方便 fixture 和单测，但不得形成共享 scan factory；
  registry、optimizer、physical execution 代码不得依赖这些 helper。

必须求值的边界包括：

- CLI/JSON/CSV/human 输出。
- `yield` 输出结果收集。
- 需要真实行数据的 inspect 函数。
- 不支持延迟执行的旧 builtin。
- 跨源 join、复杂 map、复杂 filter 等第一阶段无法下推的操作。

## Proposed Packages

用户入口统一采用 provider package 形态：`array.from` 构造内联表，
`csv.from` 读取 CSV，`sqlite.from` 扫描 SQLite 表，后续 MySQL/PostgreSQL 等外部源
也按同样模式扩展为 `mysql.from` / `postgres.from`。
不恢复或新增 universe 顶层数据源入口。

```flux
import "sqlite"

sqlite.from(
    path: "cpp/pl/flux/examples/data/demo.db",
    table: "cpu"
)
|> range(start: 2024-01-01T00:00:00Z)
|> filter(fn: (r) => r.host == "edge-1")
|> keep(columns: ["_time", "host", "usage"])
```

未来 MySQL 示例：

```flux
import "mysql"

mysql.from(
    dsn: "user:password@tcp(127.0.0.1:3306)/metrics",
    table: "cpu"
)
|> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-02T00:00:00Z)
```

## Data Model Changes

当前 `Value::Type::Table` 只代表已物化的 `TableValue`。后续有两种可选改法：

### Option A: Extend TableValue

在 `TableValue` 内增加可选 plan/source 字段：

```cpp
struct TableValue {
    std::string bucket;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    std::vector<TableChunk> tables;

    std::shared_ptr<LogicalPlan> plan;
    bool materialized = true;
};
```

优点：

- 对现有 `Value::Type::Table` 和 builtin 签名侵入较小。
- 旧 builtin 可以在入口处调用 `Materialize(table)`。

缺点：

- `TableValue` 会同时承载逻辑计划和物化数据，职责会变重。

### Option B: Add TableStreamValue

新增独立运行时类型：

```cpp
enum class Type {
    ...
    Table,
    TableStream,
};
```

优点：

- 延迟计划和物化表边界更干净。

缺点：

- 会影响大量 `require_table_property`、输出格式和测试，初期改动偏大。

建议采用 Option A：先在 `TableValue` 内加入延迟计划能力，等 plan 架构稳定后再考虑是否拆成独立类型。

## Connector Interface

建议新增目录：

```text
cpp/pl/flux/connector/
```

核心接口草案：

```cpp
struct ColumnSchema {
    std::string name;
    Value::Type type;
    bool nullable = true;
};

struct TableSchema {
    std::vector<ColumnSchema> columns;
};

struct SourceCapabilities {
    bool projection = false;
    bool filter = false;
    bool time_range = false;
    bool limit = false;
    bool sort = false;
    bool aggregate = false;
    bool distinct = false;
};

struct ScanRequest {
    std::vector<std::string> columns;
    std::vector<ProjectionColumn> projection_columns;
    std::optional<TimeRange> time_range;
    std::vector<Predicate> predicates;
    std::vector<OrderBy> order_by;
    std::vector<std::string> group_by;
    std::optional<AggregateRequest> aggregate;
    std::optional<std::string> distinct;
    std::optional<int64_t> limit;
    std::optional<int64_t> offset;
};

struct ConnectorRuntime {
    std::unique_ptr<ConnectorMetadata> metadata;
    std::unique_ptr<ConnectorSplitManager> split_manager;
    std::unique_ptr<ConnectorPageSourceProvider> page_source_provider;
};

class ConnectorPageSource {
public:
    virtual ~ConnectorPageSource() = default;
    virtual absl::StatusOr<std::optional<Page>> NextPage() = 0;
};
```

第一批实现：

- memory runtime：包装当前 `array.from` / CSV rows，通过 split/page source 对 executor 暴露。
- SQLite runtime：metadata 通过 SQLite schema/statistics，page source 持有 statement 逐页读取。
- MySQL runtime：metadata 通过 MySQL schema/statistics，page source 通过 execution state 逐批读取。

注意：仓库根目录已有 `sqlite3` bazel dependency，可优先用 SQLite 做第一条外部数据源闭环。

## Logical Plan

建议新增：

```text
cpp/pl/flux/plan/
```

Logical node 草案：

```cpp
enum class PlanNodeKind {
    SourceScan,
    Range,
    Filter,
    Project,
    Rename,
    Map,
    Limit,
    Sort,
    Group,
    Aggregate,
    Window,
    Join,
    Union,
    Yield,
    Materialize,
};

struct PlanNode {
    PlanNodeKind kind;
    std::vector<std::shared_ptr<PlanNode>> inputs;
};
```

需要重点保留的信息：

- 源数据标识：driver、dsn、query/table、bucket。
- schema 和列类型。
- Flux 原始表达式，用于 fallback。
- 可翻译 predicate，用于 pushdown。
- group key / logical table boundary。
- range_start、range_stop、result_name。

第一版 logical plan 只覆盖线性管道：

```text
SourceScan -> Range -> Filter -> Project -> Sort -> Limit -> Materialize
```

`join/union/window/aggregateWindow/pivot` 初期可以作为 materialization barrier。

## Predicate Representation

下推必须非常保守。建议先支持简单谓词：

```cpp
enum class PredicateOp {
    Eq,
    NotEq,
    Lt,
    Lte,
    Gt,
    Gte,
    RegexMatch,
    And,
    Or,
};

struct Predicate {
    PredicateOp op;
    std::string column;
    Value literal;
    std::vector<Predicate> children;
};
```

可下推表达式示例：

```flux
filter(fn: (r) => r.host == "edge-1")
filter(fn: (r) => r._value >= 80.0)
filter(fn: (r) => r.host == "edge-1" and r.region == "west")
```

不可下推表达式示例：

```flux
filter(fn: (r) => contains(set: hosts, value: r.host))
filter(fn: (r) => myPredicate(r))
filter(fn: (r) => r.host =~ /edge-.*/)
map(fn: (r) => ({r with score: r._value * 100.0}))
```

不可下推不代表失败，只代表先执行可下推前缀，再 materialize，之后回到现有内存 builtin。

## Pushdown Rules

第一批可下推：

- `range(start:, stop:)` 到 `_time` 条件。
- `filter(fn:)` 的简单列/字面量比较。
- `keep(columns:)` 到 projection。
- `drop(columns:)` 可转换成 projection，但需要 schema。
- `limit(n:, offset:)`。
- `sort(columns:, desc:)`。

第二批再考虑：

- `count/min/max/sum/mean` 简单聚合。
- `group(columns:)` + 简单 aggregate。
- 部分 `distinct(column:)`。

暂不下推：

- `map`。
- `reduce`。
- `pivot`。
- `join`。
- `window/aggregateWindow`。
- `fill/elapsed/difference/derivative`。
- 涉及用户函数、闭包、动态列名、复杂对象返回的表达式。

## SQLite SQL Builder

SQLite connector 可以把 `ScanRequest` 翻译成：

```sql
SELECT col1, col2, col3
FROM (<base query>) AS flux_source
WHERE _time >= ? AND _time < ? AND host = ?
ORDER BY _time ASC
LIMIT ? OFFSET ?
```

要求：

- 参数必须使用 bind，不拼接用户字面量。
- 列名必须来自 schema 或经过严格 identifier quote。
- connector 内部统一包成 subquery，便于追加条件。
- 用户入口只接受 `table`，表名必须走安全标识符或 quote identifier。
- `_time` 类型第一版可以支持 RFC3339 string；后续再补 epoch/int/time affinity 映射。

## Execution Strategy

计划执行分为三步：

1. 分析 plan 前缀，找出 connector 能下推的连续算子。
2. 构造 `ScanRequest`，让 connector 扫描并返回 `TableValue`。
3. 对剩余 plan 节点调用现有内存 builtin 或对应 physical operator。

示例：

```text
sqlite.from
  |> range
  |> filter(simple)
  |> keep
  |> map(complex)
  |> limit
```

历史上的执行拆分：

```text
ConnectorPageSource(range + filter + projection)
  -> Page stream
  -> in-memory map
  -> in-memory limit
```

`limit` 在 `map` 之后不能越过 `map` 随便下推，因为 `map` 可能改变行数或语义。后续 optimizer 可以在证明安全时再做规则。

## Historical Migration Plan

下面的 phase 记录保留从 eager interpreter 走到当前 connector runtime / pipeline 主干的历史。
若与上面的 Current Status Snapshot 冲突，以上面的当前状态为准。

### Phase 0: Document and Guardrails

- 新增本文档，明确多数据源演进方向。
- 暂不改运行时行为。
- 后续每个阶段都同步 README、SUPPORT_MATRIX 和测试。

### Phase 1: Minimal sqlite.from

状态：已完成 SQLite table scan 闭环。当前 `sqlite.from(path:, table:)`
会通过 SQLite C API 扫描指定表并返回内存 `TableValue`，后续 `filter/limit/...` 可继续
追加 logical plan 并参与保守 pushdown。用户 API 不提供 `query` 入口。

目标：SQLite 表能以 Flux-first 的方式进入 Flux 内存表。

工作项：

- 新增 `sqlite` package 和 `sqlite.from(path:, table:)`。
- 使用 SQLite C API 扫描表，SQL 只作为 connector 内部实现细节。
- 将 SQLite row 转成 `ObjectValue` / `TableValue`。
- 类型映射覆盖 null/int/float/text/blob-as-string。
- 增加 runtime eval/exec 单测。
- 增加一个 SQLite 示例或测试 fixture。

验收：

```flux
import "sqlite"

sqlite.from(path: "...", table: "cpu")
|> filter(fn: (r) => r.host == "edge-1")
|> limit(n: 10)
```

能通过现有内存算子继续执行。

### Phase 2: Connector Source Abstraction

状态：已完成并在 Phase 8 后收口。共享 `TableSource` 抽象已移除；新执行路径统一通过
`ConnectorRuntime` 的 metadata / split / page source 三件套。SQLite/MySQL 已实现真实 capability、
statistics 和 `ScanRequest` pushdown；array/csv 通过 memory runtime 暴露同形状 page source。

目标：统一 CSV/array/SQL 数据源入口。

工作项：

- 新增 connector source 抽象，后续演进为 `ConnectorRuntime`。
- 将 `sqlite.from` 改为构造 lazy source plan，再由 physical execution 触发 connector scan。
- 让 `array.from` 和 `csv.from` 在内部适配同一 runtime/page source 抽象。
- 保持 `Value::table` 对外行为不变。

验收：

- 现有 examples 和 conformance 不变。
- 新增 connector runtime conformance 单测。

### Phase 3: Logical Plan Skeleton

状态：已完成并进入 physical execution 主干。`plan/PlanNode` 已成为 optimizer/explain/executor
的输入；`sqlite.from` / `mysql.from` 携带 `SourceScan`，`range/filter/keep/drop/rename/limit/sort`
等会追加 logical node；`explain()` 可输出 logical / optimized logical / physical plan，并标注
pushdown、barrier 和 memory fallback。真正延迟执行已由 Phase 7 之后的 `PhysicalExecutor` 接管。

目标：表 pipeline 可以延迟表达一小段 plan。

工作项：

- 新增 plan node 类型。
- `sqlite.from` 返回带 `SourceScan` plan 的 `TableValue`。
- `range/filter/keep/limit/sort` 在输入带 plan 时追加节点。
- 输出或旧 builtin 入口调用 `Materialize`。

验收：

- 对无 SQL 数据源，现有行为保持一致。
- SQL pipeline 在 materialize 前能打印/debug 出 plan。

### Phase 4: Conservative Pushdown

状态：已完成第一版。`ScanRequest` 已有简单谓词表示，`SQLiteSource` 和 `MySQLSource` 会把 projection、
`_time` range、简单 AND 谓词、sort、limit/offset 翻译为包裹 base query 的 SQLite
或 MySQL SQL，并用安全参数/格式化执行；runtime pipeline 已能把 SQL provider `SourceScan` 之上的
`range/filter(simple)/keep/drop/rename/sort/limit` 线性前缀，以及
`distinct(column:)` 和 `group(columns:) |> count/sum/mean/min/max(column:)` 编译成 pushed-down `ScanRequest`，
遇到不可完整翻译的 plan 会保守回退到已物化的内存结果。当前 `filter(fn:)` 只提取
单参数行函数表达式中的 `r.col == literal`、`r.col != literal`、`< <= > >=` 和 `and`
组合；连续简单 `filter()` 会沿 plan 递归累积到同一个 `ScanRequest.predicates`。
`drop(columns:)` 会基于当前可见 schema 反算为 projection，并阻止后续下推 predicate
引用已不可见列。简单 `rename(columns:)` 会下推为 SQL projection alias；planner 会维护
当前 Flux 可见列到 SQL provider 源列的映射，让 rename 后的 filter/sort/projection 继续安全下推。
若 rename 产生重复可见列名，则保守插入 materialization barrier。
`distinct(column:)` 会通过当前列映射下推到 SQL provider 源列，并保留后续 projection/sort/limit
继续下推的能力。
`group(columns:)` 只有和上述简单 aggregate 组合时才会触发 SQL provider `GROUP BY` 下推；
单独 `group()` 仍保持 eager 内存结果。
`or`、函数调用、正则、`in`、跨列比较、块体函数和 `onEmpty: "keep"` 都不会下推。
`explain()` 会把已下推节点标为 `[sqlite pushdown]` / `[mysql pushdown]`，把截断点标为 `[barrier: ...]`；
当 plan 可完整编译为 SQL provider `ScanRequest` 时，还会追加 `SourcePushdown(request: ...)`
摘要，列出 projection/alias、time range、predicate 累积、distinct、group_by、
aggregate、order_by、limit/offset 等真实下推字段。

目标：SQLite/MySQL 能下推简单 range/filter/projection/limit/sort。

工作项：

- 实现 Flux filter 简单表达式提取。
- 实现 `ScanRequest`。
- 实现 SQLite SQL builder。
- 增加 explain/debug 输出，方便确认哪些算子被下推。
- 增加“不下推但结果正确”的 fallback 测试。

验收：

- 简单查询生成带 WHERE/LIMIT 的 SQL。
- 复杂 filter 自动 fallback，结果不变。
- 不允许因为下推改变 stop-exclusive range 语义。

### Phase 5: MySQL Connector

状态：已完成第一版。`mysql.from` 支持 DSN 和显式 host/user/password/database/port/ssl
连接配置，`MySQLSource` 使用 Boost.MySQL 扫描表、读取 schema、映射 MySQL 原生类型，
并复用 SQL provider pushdown 路径。当前 integration test 通过 `FLUX_MYSQL_TEST_DSN`
可选启用；GitHub Actions 可以依赖 MySQL service 运行真实 fixture。

目标：复用 connector/plan/pushdown 架构接入 MySQL。

工作项：

- 选择 C/C++ MySQL client 依赖和 Bazel 集成方式。
- 实现 MySQLSource。
- 复用大部分 SQL builder，抽出 dialect 差异。
- 增加 integration 测试策略。若 CI 无 MySQL 服务，则保留可选测试或 mock connector。

验收：

- MySQL 查询可以物化到 Flux 表。
- 简单 projection/filter/range 能按 capability 下推。

### Phase 6: Aggregation Pushdown

状态：已完成第一版。SQLite/MySQL 已支持 `distinct(column:)`，以及
`group(columns:) |> count/sum/mean/min/max(column:)` 的简单数据库侧聚合下推。

目标：对简单聚合启用数据库侧执行。

可选范围：

- `group(columns:) |> count()`
- `group(columns:) |> min/max/sum/mean(column:)`
- `distinct(column:)`

要求：

- 必须先补充逻辑表语义测试。
- SQL 聚合结果要与现有 Flux 输出 shape 对齐。
- 不确定的语义宁可 fallback。

### Phase 7: Lazy Execution and Physical Plan

状态：已完成当前阶段。当前已新增第一版 physical plan 表示、physical executor 骨架、
显式 `OutputSink` 根节点和最小 CBO 闭环。
`explain(physical: true)` 可以展示 connector scan 是否 lazy、RBO 已触发规则、CBO 当前决策状态
和基于 connector statistics 的 cost estimate。SQL source pushdown 编译已经开始从
`runtime/runtime_builtin_universe_transform.cpp` 迁出到 optimizer 层，`Materializer` 也已经改为统一进入
`PhysicalExecutor`。

当前执行路径的边界是：`PhysicalPlanner` 产出 `ExecutionTask`，其中包含 pipeline DAG；
`Scheduler` 通过 `TaskExecutor` 运行 driver task，operator 之间通过 `Page`
传递表数据。所有输出边界先进入 `OutputOperator`；完整可下推的 source plan 在其下编译成
`ConnectorScanOperator`；不能完整下推的单输入 plan 会拆成 connector pushed prefix 和 memory
suffix，memory suffix 目前覆盖 `RangeOperator`、`FilterOperator`、`ProjectOperator`、
`RenameOperator`、`LimitOperator`、`SortOperator`、`GroupOperator`、`DistinctOperator`、
`AggregateOperator`，并通过独立 `MaterializeOperator` 表达 materialization barrier。
`Page` 已经从 `TableValue` wrapper 改成独立执行页模型：page 包含 bucket/metadata、多个
`PageChunk`，chunk 内部是 `ColumnVector` 列向量和 row count，并有统一的 schema/validation
contract。`TableValue` 只在旧 builtin 和最终 `Value` 输出边界互转；
streaming operator 之间不再把 `TableValue` 当作传输协议。

当前 physical explain 示例：

```flux
data |> explain(physical: true)
```

会输出类似：

```text
PhysicalPlan
`- OutputSink [eager]
     name: "output"
     operator: "OutputOperator"
     cbo: "not-run"
     cost: unknown
   `- ConnectorScan [lazy]
        name: "connector scan"
        operator: "ConnectorScanOperator"
        source: "sqlite"
        driver: "sqlite"
        logical_prefix:
          - Limit
          - Filter
          - SourceScan
        rbo:
          - PushLimitIntoConnectorScan
          - PushPredicateIntoConnectorScan
        cbo: "chosen"
        cost: {rows=1, cpu=..., io=...}
```

这里的 `OutputSink` 是用户可观察输出边界；`[lazy]` 表示 connector scan 本身已经按延迟
scan 建模。输出边界现在会通过 `PhysicalExecutor` 触发 connector scan，并在需要时接上
memory operator fallback。
执行路径使用 fast CBO，只消费 RBO 形态决策，不会为了执行预先读取 connector 统计；physical
explain 使用完整 CBO，会在 connector 支持时读取统计并展示真实 cost，缺统计时明确显示
`cbo: "no-stats"` 和 `cost: unknown`。

目标：把当前 eager interpreter 演进成明确的 logical -> optimized logical -> physical -> execute
流程，同时保持现有内存执行器作为 fallback。

已落地：

- 让 SQL provider `from()` 返回未物化的 lazy table plan。
- Materializer 统一进入 `PhysicalExecutor`，不再直接调用 pushdown executor。
- 新增第一版 physical executor：先执行 connector pushed prefix，再把剩余 suffix 交给
  memory operator。
- `PhysicalPlan` 已补齐 `OutputSink` 节点；`explain(physical: true)` 现在稳定以
  `OutputSink` 为根，下面再展示 connector scan、memory operator 或 materialize barrier。
- 执行侧已拆出 `ExecutionTask`、`Pipeline`、`Scheduler`、`Driver`、`Page` 和 operator
  pipeline；planner 根部统一包 `OutputOperator`，materialization barrier 由独立
  `MaterializeOperator` 表达。
- memory suffix 已从单个 `MemoryUnaryOperator` 拆成 typed operators：`RangeOperator`、
  `FilterOperator`、`ProjectOperator`、`RenameOperator`、`LimitOperator`、`SortOperator`、
  `GroupOperator`、`DistinctOperator` 和 `AggregateOperator`。
- `RangeOperator`、`FilterOperator`、`ProjectOperator`、`RenameOperator`、`LimitOperator` 和
  `MaterializeOperator` 已改为 Page-native：直接变换 `PageChunk` / `ColumnVector`，不再先转成
  `TableValue`。`GroupOperator`、`DistinctOperator` 和 `AggregateOperator` 是 Page-native
  streaming accumulator；`SortOperator` 仍是明确 blocking boundary。
- 为 connector scan + memory suffix 增加 executor 级语义测试，覆盖 group 后接
  filter/project/rename/sort/limit、materialize barrier 后接 aggregate、以及 distinct fallback。
- 新增第一版 RBO pass 管线：`PlanOptimizer`、`Rule`、`RuleBasedOptimizer`、deterministic
  rule order 和 rule trace。connector pushdown rule 当前记录 trace，materialize barrier rule
  已执行真实 rewrite；默认 RBO 结果已经直接携带 connector `PushdownPlan` 和 `ScanRequest`。
  执行和 explain 都直接消费 `PlanOptimizerResult`，旧的 `optimizer/source_pushdown` 兼容层已删除。
- logical / optimized logical / physical explain 统一改从 optimizer 入口生成；`plan` 层只保留
  logical/physical IR 数据结构和通用格式化，不再判断 pushdown eligibility 或内嵌 RBO rule 名。
- `InsertMaterializationBarrier` 已成为第一条真实 RBO rewrite：当非 pushable unary logical node
  直接接在可下推 source prefix 上时，RBO 会在边界插入 materialize barrier。迁移期旧 builtin
  仍可显式构造 barrier，RBO 会稳定记录该决策。
- `explain(optimized: true)` 已支持 optimized logical plan 视图，会展示 RBO trace，并在可编译时
  附带 `SourcePushdown(request: ...)` 摘要。
- runtime builtin transform 不再执行 eager pushdown shortcut；可 lazy 的 source pipeline 会保留
  logical plan，到 materialize/output 边界再由 `PhysicalExecutor` 决定是单个 connector scan，还是
  connector pushed prefix + memory suffix。
- 新增 `optimizer/cbo.{h,cpp}`：定义 `StatsProvider`、`CostModel`、`PlanAlternative`、
  `CostBasedOptimizer` 和 `CboPlanResult`。当前 CBO 先以 RBO 输出作为合法候选形态，在有统计时
  计算 rows/cpu/io，并记录 `chosen`；无统计或执行快路径时明确 fallback 到 RBO。
- `ConnectorMetadata` 暴露轻量 `Statistics()` 接口；SQLite/MySQL 第一版通过 `COUNT(*)` 暴露
  row count，array/csv memory runtime 通过内存行数暴露统计。统计只进入 optimizer/explain 边界，
  避免污染 page source 执行路径。
- `PhysicalPlanner` 已改为消费 `FastCostBasedOptimizer()`：完整可下推计划仍编译成单个
  `ConnectorScanOperator`，不可完整下推计划仍拆成 connector prefix + memory suffix，但规划入口
  已经统一经过 CBO facade，后续切入 CBO 候选选择不需要再改 executor 边界。

待推进：

- 扩充 CBO alternative 枚举与选择逻辑：更多 connector/memory 交换点、排序/limit/aggregate 的
  多候选比较、统计缓存和列级 distinct/null fraction。

验收：

- `sqlite.from(...) |> range |> filter(simple) |> keep |> limit` 在输出边界才触发 scan。
- `sqlite.from(...) |> group` 保持 lazy logical plan，并在输出边界拆成 connector scan + memory group。
- `sqlite.from(...) |> group |> count/mean` 仍能作为整段 aggregate pushdown 执行。
- `explain(physical: true)` 能稳定展示以 `OutputSink` 为根的 physical node tree 和 RBO/CBO
  决策，包括 group-only fallback 的 memory operator + connector scan 形态。
- 所有现有 examples 输出保持不变。

### Phase 8: Presto-Style Connector Runtime

状态：已完成。当前已有 `ConnectorMetadata`、`ConnectorSplitManager`、
`ConnectorPageSourceProvider` 和 `ConnectorPageSource` 抽象；`ConnectorRegistry` 只注册 connector
runtime factory，不再暴露旧 source factory。SQLite 和 MySQL 均已接入 metadata / split /
page source runtime；SQLite page source 持有 `sqlite3_stmt` 逐页 `step`，MySQL page source 持有
Boost.MySQL execution state 逐批 `read_some_rows`。array/csv memory source 也有同形状 runtime。
`ConnectorScanOperator` 对 pushed plan 统一通过 runtime 创建 table handle、split 和 page source，
再按 split 顺序逐页产出 `Page`。`ConnectorSplit` 和 `ConnectorPageSource` 现在带 split id、
finished 状态和 page/row 统计，scan operator 消费完 page source 后会记录 split lifecycle。

目标：把 connector runtime 拆成 Presto 风格的 metadata / split / page source 三块。

工作项：

- 新增 `ConnectorMetadata`：schema、capability、statistics、table handle、column handle。SQLite/MySQL/
  memory runtime 已完成。
- 新增 `ConnectorSplitManager`：把 table handle + constraint 编译成 split 列表；SQLite/MySQL/memory
  第一版返回 single split，executor 已按 split 列表顺序调度 page source，并保留 split id、
  partition、finished 和 stats 字段，后续扩成 range/partition split 不需要再改 operator API。
- 新增 `ConnectorPageSourceProvider`：把 split + projected columns + constraints 转成 page/chunk stream。
  SQLite/MySQL page source 复用 SQL 编译，但读取层已经是真正 incremental page source，不再先扫完整表。
- `SQLiteSource` / `MySQLSource` 不再通过 registry 暴露为 planner/executor 直接调用对象；registry 和
  physical execution 入口统一为 runtime factory。
- `ArraySource` / `CsvSource` 已适配 split/page source，避免 memory source 特判污染 planner。

验收：

- SQL provider scan 通过 split/page source 执行。
- Connector runtime conformance 单测覆盖 metadata、split、多 page、empty page 和错误传播行为。
- Page 单测覆盖 columnar schema、row count validation、row materialization 和 chunk slicing。
- `ConnectorScanOperator` 覆盖 empty page source 执行路径。
- physical plan 中能展示 split 数量和 connector handle。
- 共享 `TableSource` / scan factory 已移除；registry、optimizer 或 physical execution 不依赖具体
  source helper。

### Phase 9: Scheduler / Driver / Operator Pipeline

状态：已完成单机 operator pipeline。当前已有 `ExecutionTask`、`Pipeline`、`Scheduler`、
`TaskExecutor`、`DriverTask`、`Driver`、独立 `Page`/`PageChunk`/`ColumnVector` 和 typed `Operator`；
physical planner 已经能产出 pipeline DAG。
`ConnectorScanOperator` 已能按 connector split 顺序消费 page source；`Scheduler` 可以执行多个
pipeline 并合并输出。当前调度是单机多 driver 执行，已具备本地 gather/repartition exchange
语义，但不伪装成分布式 shuffle。

目标：把 single-node 执行做成 Presto-like driver/operator pipeline，而不是递归调用 builtin。

工作项：

- 新增 `ExecutionTask`、`Pipeline`、`Driver`、`Operator`、`Page`/`Chunk` 抽象。
- 实现 `TableScanOperator`、`FilterOperator`、`ProjectOperator`、`LimitOperator`、
  `SortOperator`、`AggregationOperator`、`MaterializeOperator`、`OutputOperator`。
  当前对应实现为 `ConnectorScanOperator`、`RangeOperator`、`FilterOperator`、
  `ProjectOperator`、`RenameOperator`、`LimitOperator`、`SortOperator`、`GroupOperator`、
  `DistinctOperator`、`AggregateOperator`、`MaterializeOperator` 和 `OutputOperator`。
- physical planner 产出 pipeline，scheduler 同步执行 driver。
- streaming memory operator 已逐 page 消费/产出；group/distinct/aggregate 已改为 streaming
  accumulator，仍是 blocking result boundary，但不再拼接整表输入；sort/topn/join/materialize
  仍承担全局语义的 blocking boundary。
- 旧 eager builtin fallback 已被限制在 typed memory operators 内部，operator 之间不再递归传
  `Value`。

验收：

- `sqlite.from |> range |> filter(simple) |> keep |> limit` 走
  `ConnectorScanOperator -> OutputOperator`，scan 到输出边界才发生。
- `sqlite.from |> filter(complex) |> limit` 走
  `ConnectorScanOperator -> FilterOperator -> LimitOperator -> OutputOperator`，复杂谓词不会下推，
  但会在 memory suffix 中执行。
- `explain(physical: true)` 能展示 output/connector/memory/materialize operator tree，并标注
  具体 operator 名。

### Phase 10: Cost and Join Foundations

状态：已完成单机基础版。当前已有 logical `JoinSpec` / `ExchangeSpec`、physical
`LocalHashJoin` / `Exchange` 节点、二输入 `LocalHashJoinOperator`、本地 gather/repartition
`ExchangeOperator`、join build-side CBO 选择、多 pipeline task 描述、connector column stats
推导和 physical alternatives explain。executor 已能把两个 connector-backed inputs 接到同一个
local hash join；exchange 明确是单机 materializing boundary，不伪装成分布式 shuffle。

目标：在不伪造分布式能力的前提下，为 join、exchange 和 CBO 留好真实接口。

工作项：

- `StatsProvider` / `CostModel` 已扩展到 join/exchange；connector column statistics contract
  现在包含 NDV、null fraction 和 average width。Memory/SQLite/MySQL 已能推导这些列统计，
  无法取得的字段显式保持 unknown。
- CBO alternatives 不再只有单个 RBO facade：connector full pushdown 会同时记录 memory suffix
  fallback；connector prefix + memory suffix 会记录 memory fallback；join 会记录 local hash join
  的 build-left/build-right alternatives，并把低成本 side 写回 `JoinSpec`。
- join 先实现 local hash join physical node；跨源 join 不下推，但 executor 已能规划/执行
  多输入 operator tree。
- 新增 `Exchange` logical/physical/operator：当前 gather/repartition 已逐 Page streaming；
  repartition 会在每个输入 Page 内按 partition key 形成本地 chunks，为后续并发和远端 exchange 留接口。

已落地：

- `PlanNode` 新增 `MakeJoin`、`MakeExchange`、`JoinMethod`、`JoinBuildSide`、`ExchangeKind`。
- `PhysicalExecutor` 新增 `LocalHashJoinOperator`，按 `JoinSpec.build_side` 选择 build 表，
  保持逻辑 left/right 输出列顺序，并支持 inner/left/right/full 的基础行补齐语义。
- `PhysicalPlanner` 已能构造多输入 operator tree；Phase 11 后 join task 已进一步拆成真实
  producer/consumer pipeline DAG。
- `ExchangeOperator` 已实现本地 gather/repartition；Phase 15 后不再整表 collect，而是作为
  streaming Page boundary 在 plan/explain 中显式出现。
- `CostModel` 能估算 local hash join 和 exchange 的启发式成本；stats 可用时会自动选择更小
  build side，stats unknown 时仍清楚标记 fallback。
- `explain(physical: true)` 会展示 CBO alternatives 和 chosen 标记，便于看到被放弃的 physical
  shape/build side。

验收：

- physical executor 能执行两个 connector input 的 local hash join。
- physical planner 能展示/返回二输入 join operator tree。
- exchange gather/repartition 能作为显式 Page boundary 执行，repartition 后同一输入 page 中的
  同 key rows 位于同一个本地 chunk。
- CBO 未启用或 stats unknown 时，explain 明确显示 fallback 到 RBO。
- stats 可用时自动选择 join build side；stats unknown 时不因为不可靠 cost 改变 join side。

### Phase 11: Pipeline DAG and Local Exchange Runtime

状态：已完成单机 DAG 基础版。`PhysicalPlanner` 不再把顶层 join 的左右输入当作空 descriptor：
它会生成 producer pipelines、root consumer pipeline 和显式 dependency。producer 通过
`ExchangeSinkOperator` 写入本地 `ExchangeBuffer`，consumer 通过 `ExchangeSourceOperator`
读取 buffer 后继续执行。`Scheduler` 按 pipeline dependency DAG 分批调度，同一 ready wave
使用本地 async driver 并发执行，并把 producer 输出从最终 result 中隔离掉。

Phase 13 后，调度实现已从 ready-wave `std::async` 过渡到 `TaskExecutor` + `DriverTask`
模型；本节保留 Phase 11 的 DAG 拆分语义记录，当前 runtime 形态见 Phase 13。

目标：让 `ExecutionTask -> Pipeline DAG -> Driver -> ExchangeBuffer -> Operator` 成为真实执行路径，
为后续 remote exchange、backpressure 和多 driver 并行留出正确边界。

已落地：

- `Pipeline` 新增 shared runtime stats：pages、rows、blocked、finished、error。
- `Driver` 记录每个 pipeline 的 page/row 产出和 error/finish 状态。
- `Scheduler` 校验 missing dependency 和 dependency cycle，按 DAG wave 调度 runnable pipelines。
- local hash join 被拆为 producer/consumer pipelines：left/right 写 exchange buffer，main
  依赖 producer 完成后读取 buffer；nested join 会递归生成 producer DAG。
- root `Exchange` 也会拆成 source producer + exchange consumer，而不是继续单 pipeline 特判。
- `ExchangeSourceOperator` 在 producer 未完成时返回 blocked/unavailable 状态；当前 scheduler
  通过 dependency 保证 consumer 不会提前运行。

验收：

- physical planner 返回真实可运行的 join-left/join-right/main 三段 pipeline。
- scheduler 执行 join pipeline DAG 后只返回 root pipeline 输出，不把 producer scan 输出混入结果。
- producer/root pipeline stats 能反映各自处理的 pages 和 rows。
- 非 join 查询仍走原本单 main pipeline，不为普通 scan/filter/project 额外收集 connector stats。

### Phase 12: Runtime Profile and Observable Exchange Core

状态：已完成单机可观测版。执行结果现在可以通过 `Scheduler::RunWithProfile` 或
`PhysicalExecutor::ExecuteWithProfile` 返回 `SchedulerResult`，其中包含最终 `Value` 和
`ExecutionProfile`。pipeline graph 和 runtime profile 有独立 formatter，可直接诊断 pipeline DAG、
operator 列表、dependency、pages、rows、blocked、finished 和 error。

目标：把 runtime stats / exchange / scheduler 做成可解释、可诊断、能承载后续远端 exchange
和更细粒度 backpressure 的执行内核。

已落地：

- 新增 `PipelineProfile`、`ExecutionProfile`、`SchedulerResult`，profile 不再只能从测试内部拿。
- 新增 `execution::FormatPipelinePlan` 和 `execution::FormatExecutionProfile`，展示 pipeline DAG
  和 runtime stats。
- `ExchangeBuffer` 补生命周期状态：bounded capacity、closed、finished、error；producer error
  会写入 buffer，consumer 读取时会传播。Phase 13 后 buffer 已升级为 blocking queue，
  backpressure 通过 condition_variable 阻塞/唤醒生产者和消费者。
- `Scheduler` 的 missing dependency / dependency cycle 错误被显式测试覆盖。
- pipeline breaker 不再只处理顶层 join：nested join 会递归拆成 producer DAG，root exchange 会拆成
  producer + consumer DAG。

验收：

- `ExecuteWithProfile(join)` 能返回最终 join 结果和三段 pipeline stats。
- `FormatPipelinePlan(join)` 能展示 `join-left`、`join-right`、`main` 及 dependency。
- nested join planner 能生成 `join-left-left`、`join-left-right`、`join-left`、`join-right`、`main`
  五段 DAG，并能正确执行。
- root exchange planner 能生成 `exchange-input -> main` DAG，并保持 gather/repartition 语义。
- blocked operator 会在 shared pipeline stats 中留下 blocked/error/finished 状态。

### Phase 13: TaskExecutor, DriverTask, and Streaming Exchange Foundation

状态：已完成单机 driver scheduler 骨架版。`Scheduler` 不再直接用 `std::async` 按 wave
执行 pipeline，而是先校验 pipeline dependency DAG，再通过独立 `TaskExecutor` 提交
`DriverTask`。`Pipeline` 现在可以承载多个 driver root，开始从“一个 pipeline 等于一个
driver 实例”演进为“pipeline 是执行模板，driver task 是运行实例”。

目标：把 runtime scheduler 推向最终形态的不可返工骨架：固定 worker pool、driver factory、
split-level driver roots、streaming exchange，以及线程安全 runtime stats。

已落地：

- 新增独立 `execution/task_executor.h|cpp`，提供固定 worker pool、`Submit()` future、
  shutdown/drain 语义；线程池实现不堆在 `physical_executor` 中。
- 新增 `DriverTask` / `DriverFactory`，`Pipeline` 支持 `driver_roots`，同一 pipeline 可以
  展开成多个 driver 实例并共享 pipeline stats。
- `Scheduler` 先做 missing dependency / cycle 校验，再把所有 driver task 交给
  `TaskExecutor`；producer/consumer 不再靠 scheduler wave 顺序串行等待。
- `Driver::RunToSink()` 允许 driver 按 page 推给 sink；`Scheduler` 聚合最终 root 输出时不再要求
  每个 driver 先构造一个完整 `Value`，producer pipeline 也可以直接丢弃本地返回值。
- `ExchangeBuffer` 改成 mutex + condition_variable 的 streaming queue；producer 写 page，
  consumer 可同时阻塞式读取 page，finish/error 会唤醒等待方。
- `Operator::Cancel()` 会沿 `Output/Exchange/Unary/Join` 链路向上游传播；root sink 或 consumer
  出错时会关闭相关 `ExchangeBuffer`，producer 不会因为队列满而永久阻塞。scheduler 等待所有
  driver 收尾后，优先返回 root/output 侧原始错误。
- connector full-pushdown pipeline 已能在 split 数大于 1 时生成多个
  `ConnectorScanOperator` driver roots；Phase 15 后 SQLite streaming scan 也已接入 multi split。
- pipeline breaker 会沿 unary spine 选择最深的 blocking 边界，先把其输入拆成 producer
  pipeline，再由 root pipeline 通过 `ExchangeSourceOperator` 消费；`group/distinct/sort/materialize`
  不再强迫其下游 connector scan 回到单 driver。
- pipeline stats 写入加锁，多个 driver 会聚合同一 pipeline 的 pages/rows/error/finish 状态。

验收：

- `TaskExecutor` 能并发执行提交任务并返回 future 结果。
- 一个 pipeline 下多个 driver root 的输出会被 scheduler 聚合为最终 root 结果。
- join / nested join / root exchange 仍能通过 streaming `ExchangeBuffer` 正确执行。
- root sink 早停会取消 exchange producer；覆盖 6000 个 1-row page、buffer 会触发背压的回归用例。
- `group/distinct/materialize` 这类 blocking unary operator 能形成真实 producer/root pipeline DAG。
- missing dependency / dependency cycle 仍在运行前被确定性拒绝。
- 全量 `bazel test //cpp/pl/flux/...` 通过。

### Phase 14: Parallel Scan and Streaming Query Fast Path

状态：已完成内存 connector 的并行 scan fast path。执行路径不再只证明“pipeline 能并发”，而是
真正让 connector split 展开为多个 `ConnectorScanOperator` driver root，并让 page source 直接
产出列式 `PageChunk`。本阶段先用 memory connector 作为可控 fixture 承担多 split、
projection/filter/limit pushdown 和大规模扫描压测；SQLite/MySQL 的真实 multi-split 已在
Phase 15 补齐。

目标：优先把高效查询扫描链路做扎实：`ConnectorSplitManager -> ConnectorPageSourceProvider ->
ConnectorScanOperator -> DriverTask -> TaskExecutor` 全链路 streaming page，不回退到整表
`TableValue` 物化后再切 page。

已落地：

- `ConnectorSplit` 增加 row range metadata：`row_offset` 和 `row_limit`，用于 split-level
  page source 定位扫描区间。
- `MemorySplitManager` 支持按目标 split 数拆分 row range；含全局 `limit` 的请求保持 single
  split，以避免跨 split limit 语义漂移。
- `MemoryPageSource` 直接构造列式 `PageChunk`，在 page source 内执行 projection、简单
  predicate、offset/limit，并保留 empty page 行为。
- memory connector capabilities 明确声明 projection/filter/limit，可作为后续高效 scan
  路径的 conformance fixture。
- physical profile 增加 `blocking=true|false`，把 sort/group/aggregate/distinct/join/exchange/
  materialize 这类 pipeline breaker 和纯 streaming scan/filter/project 区分开。
- 新增真实规模 scan 压测 fixture：20 万行内存数据、8 split、filter + project，验证输出
  row count、profile row count、page 数和非 blocking 属性，并打印 rows/s。

验收：

- connector conformance 覆盖 memory runtime 多 split、split row range、pushdown projection/
  filter/limit。
- runtime 覆盖大规模 streaming scan benchmark，当前本机结果约为 20 万输入行、10 万输出行、
  8 split、约 5.0M rows/s。
- `ExecutionProfile` 能显示 pipeline 是否 blocking，后续可直接定位是否进入高吞吐 streaming
  路径。

### Phase 15: SQLite Multi-Split Scan and Query Throughput Baseline

状态：已完成 SQLite 真实 connector 的并行 streaming scan，并扩展到 MySQL range split。SQLite
不再只作为 single split SQL page source：在不含全局 order/limit/offset/aggregate/distinct/group
的 scan/filter/project/range 路径上，split manager 会按 SQLite `rowid` 区间拆成多个 split，
并展开为多个 `ConnectorScanOperator` driver root。MySQL split manager 会优先选择主键，
再选择 `id`/`seq`/首个整型列，通过 `MIN/MAX/COUNT` 生成 range split；含全局语义的查询保持
single split，避免跨 split 后改变排序、限制或聚合语义。

目标：把高效查询扫描从 fixture 推进到真实本地数据库：SQLite page source 直接产列式
`PageChunk`，执行层常见 `range/filter/project` 保持列式处理，exchange queue 有 page/byte
预算背压，benchmark 有单独 target 能构造真实大表并输出吞吐。

已落地：

- `SQLiteSplitManager` 基于 `MIN(rowid), MAX(rowid), COUNT(*)` 做保守 split discovery；
  不支持 rowid 的表或需要全局 SQL 语义的请求自动回退 single split。
- `MySQLSplitManager` 基于主键或整型列做保守 range discovery；不能证明可拆分、或请求带全局
  SQL 语义时自动回退 single split。
- `SQLitePageSource` 支持 split rowid range，并在非 aggregate 路径直接填充列式
  `PageChunk`，不再先构造整页 `ObjectValue` rows。
- `MySQLPageSource` 支持 split column range，把 split predicate 合入 page source SQL，
  对 executor 暴露的仍是标准 `ConnectorPageSource`；非 aggregate 路径直接填充列式
  `PageChunk`，避免先构造整页 `ObjectValue` rows。
- MySQL metadata/statistics 和 split extent 有进程内缓存；多次 benchmark
  或同一进程重复规划时不再反复打 schema/count/min/max 固定开销。
- SQL connector pushdown 有统一 contract：在 SQL 生成前集中校验 projection、predicate、
  time range、sort、distinct、group/aggregate、limit/offset 和 schema column；通用 SQL builder
  同时支持 literal SQL 和 `ParameterizedSql`，MySQL page source 默认走 server-side prepared
  statement，SQLite 继续使用已有的本地参数绑定。
- MySQL page source 默认使用 server-side prepared statement，但 streaming read 固定使用独立连接；
  自研 connection pool 已撤掉，runtime 只保留 Boost.MySQL 官方 `connection_pool` 的薄适配器。
  实测官方 `pooled_connection` + dynamic `execution_state/read_some_rows` 在 ASAN 下仍会触发
  Boost.MySQL 内部 container-overflow，因此没有保留测试级 `ASAN_OPTIONS` override，也不提供
  pooled streaming 构造入口。
- MySQL split discovery 优先使用 schema 中的 `id` / `seq` / integer columns，避免 benchmark
  热路径每次先查 `INFORMATION_SCHEMA`；metadata / split discovery 走 Boost.MySQL pool 和进程内
  cache。
- 执行层 `range/filter/project` 直接从 `PageChunk` 读取列值，filter 会预解析列索引和 literal，
  避免 hot path 上逐行 `RowFromPageChunk`。
- root `ExchangeOperator` 不再 collect 完整输入后一次性输出；gather/repartition 都逐页消费
  上游 Page 并逐页产出下游 Page，本地 ExchangeBuffer 继续承担 pipeline 间有界队列和背压。
- `ExchangeBuffer` 增加 buffered page/byte budget，producer 在 queue 满或预算不足时阻塞，
  单个超大 page 允许通过，避免死锁。
- connector split profile 增加 bytes、wall time 和 metadata/split/connect/schema/sql/execute/read/
  decode/page-build 分段耗时，profile/explain formatter 与 benchmark 都会输出这些字段，用于判断
  吞吐瓶颈发生在 page source、远程服务还是后续 operator。
- 本地 blocking accumulator 已从物理执行器主文件拆到 `execution/accumulator.{h,cpp}`。group 和
  distinct 使用 typed key/hash state，不再在 hot path 上拼接 string canonical key；单列 key 有
  vector-free fast path。aggregate 对整列走 column fast path，`group |> aggregate` 会融合成
  grouped accumulator；多 driver 输入会进一步拆成 partial/global 两阶段。
- `PipelineProfile` 增加 `accumulator_stats`，覆盖 input/output rows、group buckets、
  key/hash/update/result build 分段耗时，以及 accumulator phase、key strategy、partial/final
  输入行数和耗时；`FormatExecutionProfile` 和 connector benchmark runner 都会暴露这些字段，
  用来定位本地 accumulator 的 CPU 成本。
- `//cpp/pl/flux/benchmark:sqlite_scan_benchmark` 可构造真实 SQLite 数据库并执行
  `scan`、`filter_project`、`wide_filter`、`topn`、`group_count`、`group_sum`、`group_mean`、
  `distinct_host` 等 scenario，
  输出 rows、scenario、drivers、pages、split bytes、split wall time、profile 分段耗时、
  blocking、seconds 和 rows/s。`group_count` / `distinct_host` 用 materialize barrier 固定走
  本地 accumulator，用来观察 Page-native blocking result boundary 的真实成本。
- `run_connector_benchmarks.py --prepare-mysql-benchmark-table` 可按 MySQL DSN 自动重建
  `flux_bench_cpu` 这类同结构大表，避免手写 SQL 准备 benchmark fixture。
- `run_connector_benchmarks.py` 可切换 MySQL prepared on/off、target splits 和 rows per page，
  用同一张大表沉淀 scan 性能矩阵；也支持 `--build --bazel-config release --output ...`，
  用固定 release binary 口径保存稳定 JSON baseline。

验收：

- SQLite connector 单测覆盖 rowid multi-split 和 page source 汇总行数。
- MySQL connector 集成测试在提供 `FLUX_MYSQL_TEST_DSN` 时覆盖 numeric range split 和 page source
  汇总行数。
- runtime join/profile 测试已适配 multi-driver producer pipeline，不再假设 SQLite scan 单页单 root。
- 本机真实 SQLite scan benchmark：
  - 100k rows：8 drivers，50k output rows，56 pages，median 约 0.0088s。
  - 1M rows：8 drivers，500k output rows，496 pages，约 0.0618s - 0.0658s。
  - 5M rows：8 drivers，2.5M output rows，2448 pages，约 0.271s。
  - 2026-05-17 复验：1M `scan`，8 drivers，100 万 output rows，984 pages，`1.1252s`；
    1M `filter_project`，8 drivers，50 万 output rows，496 pages，`0.2749s`，约 `3.64M`
    input rows/s，非 blocking。
  - 2026-05-17 accumulator 复验：1M `filter_project`，8 drivers，50 万 output rows，496 pages，
    `0.0577s`，约 `17.3M` input rows/s，非 blocking；1M `distinct_host`，8 drivers，64 output
    rows，985 pages，`0.0867s`，约 `11.5M` input rows/s，blocking accumulator；1M
    `group_count`，8 drivers，64 output rows，985 pages，`0.3851s`，约 `2.60M` input rows/s；
    1M `group_sum`，`0.3820s`，约 `2.62M` input rows/s；1M `group_mean`，`0.3947s`，约
    `2.53M` input rows/s。
  - 2026-05-17 typed accumulator 复验：1M `group_count`，64 output rows，985 pages，
    `0.3244s`，accumulator `key=118.45ms/hash=33.31ms/update=16.34ms`；1M `group_sum`
    `0.3224s`；1M `group_mean` `0.3218s`；1M `distinct_host` `0.2576s`。
  - 2026-05-17 release two-stage grouped accumulator 复验：1M `group_count`，64 output rows，
    9 pages，`0.0940s`，partial rows `1,000,000`，final rows `512`；1M `group_sum`
    9 pages，`0.0761s`；1M `group_mean` 9 pages，`0.1119s`；1M `filter_project`
    `0.0390s`，约 `25.67M` input rows/s。
- 远程 MySQL 同结构 scan benchmark：
  - 表：`flux_bench_cpu(_time, host, region, usage, seq primary key)`，数据生成逻辑与 SQLite
    benchmark 一致。
  - 100k rows：8 drivers，50k output rows，53-55 pages，median 约 0.0693s。
  - 1M rows：8 drivers，500k output rows，487-488 pages，约 0.207s / 0.239s / 0.338s。
  - 5M rows：8 drivers，2.5M output rows，2412 pages，约 1.208s。
  - 结论：同样 `filter_project` 查询下，远程 MySQL 端到端仍明显慢于本地 SQLite；新的分段 profile
    显示主要成本在远程 read、协议 decode、execution start 和 split discovery，Page build 不再是
    首要瓶颈。
- MySQL prepared statement 矩阵：
  - 5M rows，8 drivers，rows/page=2048，prepared on median `1.2069s`。
  - 同表同查询，prepared off median `1.2799s`。
  - 结论：prepared 默认开启是合理的，但这组远程查询的主瓶颈仍是 read/decode，而不是 SQL build 或
    prepare/execute。

## Test Plan

测试分层：

- `runtime/tests/runtime_eval_unit_test.cpp`：表达式和 package 调用。
- `runtime/tests/runtime_exec_unit_test.cpp`：Flux 文件执行、pipeline 行为。
- `connector/tests/*_unit_test.cpp`：SQLite/MySQL query、类型映射、错误处理、SQL builder。
- `optimizer/tests/*_unit_test.cpp`：RBO/CBO 规则、trace 和 cost fallback。
- `cli/tests/flux_cli_unit_test.cpp`：CLI 输出和 examples 兼容。

关键测试场景：

- SQLite 基础查询返回 int/float/string/null。
- `sqlite.from |> filter |> limit` 走内存 fallback。
- `sqlite.from |> range |> filter(simple) |> keep |> limit` 下推。
- 复杂 filter 不下推但结果正确。
- SQLite 错误能返回可诊断的 `absl::Status`。
- 空结果、空表、null 值、缺失列。
- `_time` stop-exclusive。

## Documentation Plan

每阶段同步：

- `README.md`：用户入口和示例。
- `SUPPORT_MATRIX.md`：真实支持状态。
- 本文档：阶段状态、设计调整和新增风险。
- 如有 benchmark，再同步 `benchmark/README.md` 和 `benchmark/OPTIMIZATION_LOG.md`。

## Open Questions

- MySQL prepared statement 是否值得进入热路径？当前结论是暂不做：scan SQL 仍由 pushdown contract
  生成完整 dialect SQL，split range 会改变 SQL text；真正收益需要先把 SQL builder 改成参数化
  expression tree，再引入 per-connection statement cache。
- `_time` 在 SQL 中第一版已支持 SQLite RFC3339 text 和 MySQL 原生 datetime/timestamp；是否继续扩展 epoch/int/time affinity 映射？
- `TableValue` 仍承载 lazy plan metadata，用于旧 builtin/输出边界；是否在后续阶段彻底拆出
  `TableStreamValue`，让 Flux value 模型和 execution stream 模型完全分离？
- MySQL integration test 在当前 Bazel/CI 环境中如何稳定运行，是否把 benchmark fixture 自动化纳入
  CI nightly 而不是普通单测？
- CBO 的 statistics 来源来自 connector schema、采样、显式 analyze，还是先只做 rule-cost placeholder？
- Page source 的 batch decode 和 string/value allocation 是否需要进一步进入统一 runtime allocator /
  memory pool？

## Recommended Next Step

下一轮继续优先做高效查询扫描主干，而不是扩 CBO/join：

- MySQL 更大规模 scan：用 5M/10M fixture 继续观察 direct streaming read/execute/decode 的比例，
  同时单独跟踪 Boost.MySQL pooled dynamic streaming 的 ASAN 上游问题。
- SQL 参数化设计：如果后续要做 prepared statement cache，先把 `BuildScanSql` 产物从 literal SQL
  升级成 SQL text + bind vector，否则 statement cache 会因为每个 split range 的 SQL text 不同而
  命中率很低。
- Page source 内存优化：继续减少 `Value` string/blob allocation，并评估是否需要 connector-local
  arena。
- 只有 scan/filter/project/range 主干稳定后，再推进 join/exchange 优化和更激进 CBO alternatives。
