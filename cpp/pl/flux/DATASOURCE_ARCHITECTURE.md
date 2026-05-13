# Flux Data Source Architecture Plan

本文档记录 `cpp/pl/flux` 后续扩展多数据源能力的设计方向。目标是先支持
SQLite/MySQL 这类外部表数据源，再逐步演进到带查询计划、算子下推和内存回退的执行架构。

当前 `flux` 运行时是 eager interpreter：`csv.from`、`array.from`、`sqlite.from` 直接构造或扫描
`TableValue`，后续 `range`、`filter`、`map`、`group`、`aggregateWindow` 等 builtin
立即遍历内存表并返回新的 `TableValue`。这个模型适合 examples、单机测试和小数据集，
但接入数据库后会自然遇到大表全量拉取、无法利用索引、无法复用数据库聚合能力等问题。

设计原则是渐进式演进：先把数据源接进来，再抽象 connector，再引入 logical plan 和
pushdown。现有内存执行器不应被推倒重写，而应成为所有无法下推算子的可靠 fallback。

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
- scheduler 即使第一版只单线程同步执行，也保留 task/split/driver 边界。
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

## Non-Goals

- 第一阶段不实现 Presto/Trino 级别的完整分布式查询引擎。
- 第一阶段不要求 Flux 任意函数表达式都能翻译成 SQL。
- 第一阶段不做跨数据源 join 下推、复杂窗口下推或 cost-based optimizer。
- 第一阶段不改变 parser 语法，只新增 package/builtin 和运行时执行结构。

## Current Architecture

当前主要数据流如下：

```text
Flux source
  -> parser AST
  -> ExpressionEvaluator / StatementExecutor
  -> builtin function callback
  -> TableValue rows/tables
  -> next builtin callback
  -> output formatter
```

表数据入口：

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

这个结构的优点是简单、可调试、测试覆盖集中。缺点是数据源一旦变大，查询只能先落到内存。

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
- scheduler 把 split 分配给 driver；第一版可以只有一个 task、一个 split、同步执行。
- connector 能处理的前缀在 source 执行；不能处理的后缀通过 memory operator 执行。

截至目前，connector、logical plan skeleton、SQLite/MySQL 保守下推和简单聚合下推已经落地。
接下来优先级不再是继续扩展更多数据源，而是把执行链路补成更接近完整查询引擎的形态：

- 数据源入口返回真正 lazy 的 table plan，而不是先物化再附加 plan metadata。
- logical plan 先进入 RBO，做确定性的下推、barrier、projection pruning 等规则优化。
- RBO 输出的 logical plan 编译成 physical plan，明确区分 connector scan、memory operator、
  materialize barrier 和 output sink。
- CBO 先以框架形式存在，保留 statistics/cost/alternative plan 的接口；在没有统计信息时明确
  退化为 RBO 决策，不伪造精度。
- physical executor 负责拆分 pushed prefix 和 memory suffix，逐步替换当前 scattered
  在 builtin 内部的 eager/pushdown 混合逻辑。

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
- Scheduler：把 physical plan 转为 task/pipeline/driver。单机第一版可以同步执行，但 API
  不能假设永远只有一个 driver。
- Operator：消费和产生 page/chunk。row-by-row 可以作为内部实现细节，但不能成为长期跨层接口。
- Connector：拆成 metadata、split manager、page source provider。当前 `TableSource::Scan`
  是兼容层，后续要逐步退到 connector 内部实现。
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
- 允许迁移期保留兼容 wrapper，但新代码必须朝上述边界收敛。

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

class TableSource {
public:
    virtual ~TableSource() = default;
    virtual absl::StatusOr<TableSchema> Schema() const = 0;
    virtual SourceCapabilities Capabilities() const = 0;
    virtual absl::StatusOr<Value> Scan(const ScanRequest& request) = 0;
};
```

第一批实现：

- `ArraySource`：包装当前 `array.from` rows。
- `CsvSource`：包装当前 CSV 解析结果。初期仍可物化，后续可做流式解析。
- `SQLiteSource`：基于 SQLite C API，将 `ScanRequest` 翻译为 SQL。
- `MySQLSource`：可以先预留接口，真正依赖和构建方式后续单独评估。

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

执行拆分：

```text
SQLiteSource.Scan(range + filter + projection)
  -> materialized TableValue
  -> in-memory map
  -> in-memory limit
```

`limit` 在 `map` 之后不能越过 `map` 随便下推，因为 `map` 可能改变行数或语义。后续 optimizer 可以在证明安全时再做规则。

## Migration Plan

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

### Phase 2: TableSource Abstraction

状态：已完成。当前已新增 `connector/TableSource` 接口、`SQLiteSource`、`MySQLSource`、
`ArraySource` 和 `CsvSource`，并将 provider 入口接到对应 source。SQLite/MySQL 已实现
真实 capability 和 `ScanRequest` pushdown；array/csv 仍是内存 source。

目标：统一 CSV/array/SQL 数据源入口。

工作项：

- 新增 connector 接口。
- 将 `sqlite.from` 改为 `SQLiteSource.Scan({})`。
- 让 `array.from` 和 `csv.from` 至少在内部可适配同一抽象。
- 保持 `Value::table` 对外行为不变。

验收：

- 现有 examples 和 conformance 不变。
- 新增 connector 层单测。

### Phase 3: Logical Plan Skeleton

状态：已启动。当前已新增 `plan/PlanNode` skeleton，`TableValue` 可携带可选 `plan`，
`sqlite.from` 的物化结果会附带 `SourceScan` 节点；当输入 table 已有 plan 时，
`range/filter/keep/limit/sort` 会在保持 eager 内存执行结果不变的同时追加
`Range/Filter/Project/Limit/Sort` 节点；`explain()` 可输出已记录的 logical plan 文本，
并标注每个节点当前会走 `sqlite pushdown`、`sqlite scan`、`barrier` 还是 `memory`。
遇到第一阶段暂不支持延迟的 table builtin 时，会在 plan 中插入带原因和 builtin 名称的
`Materialize` barrier，明确截断后续下推边界；真正的延迟执行仍未实现。

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

当前执行路径的边界是：所有输出边界先进入 `OutputOperator`；完整可下推的 source plan 在其下
编译成单个 `ConnectorScanOperator`；不能完整下推的单输入 plan 会递归拆成 connector pushed
prefix 和 memory suffix，memory suffix
目前覆盖 `range`、`filter`、`project`、`rename`、`limit`、`sort`、`group`、`distinct`、
`aggregate`，并通过独立 `MaterializeOperator` 表达 materialization barrier。operator
之间仍传递 `TableValue`，这是迁移期兼容层；后续
不能在这个接口上继续扩散新能力，而要收敛到 page/chunk execution contract。

当前 physical explain 示例：

```flux
data |> explain(physical: true)
```

会输出类似：

```text
PhysicalPlan
`- OutputSink [eager](name="output", cbo="not-run", cost=unknown)
   `- ConnectorScan [lazy](name="connector scan", source="sqlite", driver="sqlite",
      logical_prefix=[Limit, Filter, SourceScan],
      rbo=[PushLimitIntoConnectorScan, PushPredicateIntoConnectorScan],
      cbo="chosen", cost={rows=1, cpu=..., io=...})
```

这里的 `OutputSink` 是用户可观察输出边界；`[lazy]` 表示 connector scan 本身已经按延迟
scan 建模。输出边界现在会通过 `PhysicalExecutor` 触发 connector scan，并在需要时接上
memory operator fallback。
执行路径使用 fast CBO，只消费 RBO 形态决策，不会为了执行预先读取 connector 统计；physical
explain 使用完整 CBO，会在 connector 支持时读取统计并展示真实 cost，缺统计时明确显示
`cbo="no-stats", cost=unknown`。

目标：把当前 eager interpreter 演进成明确的 logical -> optimized logical -> physical -> execute
流程，同时保持现有内存执行器作为 fallback。

已落地：

- 让 SQL provider `from()` 返回未物化的 lazy table plan。
- Materializer 统一进入 `PhysicalExecutor`，不再直接调用 pushdown executor。
- 新增第一版 physical executor：先执行 connector pushed prefix，再把剩余 suffix 交给
  memory operator。
- `PhysicalPlan` 已补齐 `OutputSink` 节点；`explain(physical: true)` 现在稳定以
  `OutputSink` 为根，下面再展示 connector scan、memory operator 或 materialize barrier。
- 执行侧已拆出 `OutputOperator` 和 `MaterializeOperator`，planner 根部统一包输出 operator，
  materialization barrier 不再混在通用 memory unary operator 分支里。
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
- `TableSource` 新增轻量 `Statistics()` 接口；SQLite/MySQL 第一版通过 `COUNT(*)` 暴露 row count，
  array/csv source 通过内存行数暴露统计。统计只进入 optimizer/explain 边界，避免污染 scan
  执行路径。
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

状态：未开始。当前 `TableSource::Scan(ScanRequest)` 仍是 connector 主接口，足够支撑第一版
pushdown，但不适合作为长期执行引擎边界。

目标：把 connector runtime 拆成 Presto 风格的 metadata / split / page source 三块。

工作项：

- 新增 `ConnectorMetadata`：schema、capability、statistics、table handle、column handle。
- 新增 `ConnectorSplitManager`：把 table handle + constraint 编译成 split 列表；SQLite/MySQL
  第一版返回单 split。
- 新增 `ConnectorPageSourceProvider`：把 split + projected columns + constraints 转成 page/chunk stream。
- `SQLiteSource` / `MySQLSource` 保留为 page source 内部实现，不再暴露为 planner 直接调用对象。
- `ArraySource` / `CsvSource` 也适配 split/page source，避免 memory source 特判污染 planner。

验收：

- SQL provider scan 通过 split/page source 执行。
- physical plan 中能展示 split 数量和 connector handle。
- `TableSource::Scan` 只作为兼容 wrapper 或测试 helper。

### Phase 9: Scheduler / Driver / Operator Pipeline

状态：已启动迁移期骨架。当前已有 `Operator`、`Driver`、`ConnectorScanOperator`、
`MemoryUnaryOperator`、`MaterializeOperator` 和 `OutputOperator`；physical planner 已经能产出
同步执行的单 root operator tree。尚未引入 `ExecutionTask`、`Pipeline`、`Page`/`Chunk` 和真正的
pipeline scheduler。

目标：把 single-node 执行做成 Presto-like driver/operator pipeline，而不是递归调用 builtin。

工作项：

- 新增 `ExecutionTask`、`Pipeline`、`Driver`、`Operator`、`Page`/`Chunk` 抽象。
- 实现 `TableScanOperator`、`FilterOperator`、`ProjectOperator`、`LimitOperator`、
  `SortOperator`、`AggregationOperator`、`MaterializeOperator`、`OutputOperator`。
  其中 `ConnectorScanOperator`、`MaterializeOperator`、`OutputOperator` 和迁移期
  `MemoryUnaryOperator` 已落地；按算子拆分的 memory operators 仍待继续推进。
- physical planner 产出 pipeline，scheduler 同步执行 driver。
- 旧 eager builtin 作为 `MaterializeOperator` 后的 fallback path。

验收：

- `sqlite.from |> range |> filter(simple) |> keep |> limit` 走
  `TableScanOperator -> OutputOperator`，scan 到输出边界才发生。
- `sqlite.from |> filter(complex) |> limit` 走
  `TableScanOperator -> MaterializeOperator -> FilterOperator -> LimitOperator -> OutputOperator`。
- `explain(physical: true)` 目前能展示 output/connector/memory/materialize operator tree；
  后续补充 pipeline/driver/operator 的更细粒度展示。

### Phase 10: Cost and Join Foundations

状态：未开始。

目标：在不伪造分布式能力的前提下，为 join、exchange 和 CBO 留好真实接口。

工作项：

- 新增 `StatsProvider`、`CostCalculator`、`PlanEnumerator`、`CostComparator`。
- connector statistics 支持 unknown / connector-provided / derived 三种来源。
- join 先实现 local hash join physical node；跨源 join 不下推，但 physical plan 中显式展示
  多 source pipeline + local exchange/materialize + join。
- 新增 `LocalExchangeNode` 占位，为后续并发和 pipeline 分离做准备。

验收：

- MySQL + SQLite + CSV 跨源 example 的 physical explain 能看到多个 source pipeline 和 local join。
- CBO 未启用或 stats unknown 时，explain 明确显示 fallback 到 RBO。
- 不因为不可靠 cost 自动改变 join 顺序。

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

- DSN 是否允许从环境变量读取，避免示例里出现敏感信息？
- `_time` 在 SQL 中第一版已支持 SQLite RFC3339 text 和 MySQL 原生 datetime/timestamp；是否继续扩展 epoch/int/time affinity 映射？
- `TableValue` 内嵌 plan 是否会让值模型过重，未来是否需要拆出 `TableStreamValue`？
- MySQL integration test 在当前 Bazel/CI 环境中如何稳定运行？
- CBO 的 statistics 来源来自 connector schema、采样、显式 analyze，还是先只做 rule-cost placeholder？

## Recommended Next Step

下一轮建议继续 Phase 7：把 pushdown 编译逻辑从 builtin helper 中迁到 optimizer/physical planner，
并让 SQL provider 入口返回真正 lazy 的 plan。

这样可以先把查询引擎主干补齐，再决定 PostgreSQL、HTTP、Parquet 等新数据源如何接入。
