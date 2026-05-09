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

它会借鉴 Presto/Trino 的核心查询引擎思想：

- 统一语言入口。
- connector abstraction。
- logical plan。
- rule-based optimizer。
- connector capability。
- operator pushdown。
- physical operators。
- local fallback execution。

但它不是完整 Presto，也不以分布式执行为目标。暂不考虑：

- coordinator / worker 架构。
- split scheduling。
- distributed exchange。
- shuffle。
- cross-node fault tolerance。
- 分布式 resource group / cluster management。

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

目标结构分为五层：

```text
Flux AST
  -> Runtime / builtin binding
  -> LogicalPlan
  -> Optimizer / Pushdown
  -> Physical execution
       - Connector scan
       - In-memory operators
       - Output materialization
```

核心思想：

- 数据源 builtin 不立即全量读取，而是尽量返回一个可延迟执行的表计划。
- 表变换 builtin 优先把自己追加为 logical operator。
- 当遇到必须求值的边界时，再将 plan 编译成 physical execution。
- connector 能处理的前缀下推给数据源；不能处理的后缀在内存执行。

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

状态：已启动第一块。当前已新增 `connector/TableSource` 接口、`SQLiteSource`、
`ArraySource` 和 `CsvSource`，并将 `sqlite.from`、`array.from`、`csv.from` 改为通过
对应 source 的 `Scan({})` 物化，外部行为保持不变。真实 capability、`ScanRequest`
pushdown 仍未实现。

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

状态：已启动。`ScanRequest` 已有简单谓词表示，`SQLiteSource` 会把 projection、
`_time` range、简单 AND 谓词、sort、limit/offset 翻译为包裹 base query 的 SQLite
SQL，并用 bind 参数执行；runtime pipeline 已能把 SQLite `SourceScan` 之上的
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

目标：对简单聚合启用数据库侧执行。

可选范围：

- `group(columns:) |> count()`
- `group(columns:) |> min/max/sum/mean(column:)`
- `distinct(column:)`

要求：

- 必须先补充逻辑表语义测试。
- SQL 聚合结果要与现有 Flux 输出 shape 对齐。
- 不确定的语义宁可 fallback。

## Test Plan

测试分层：

- `runtime_eval_unit_test.cpp`：表达式和 package 调用。
- `runtime_exec_unit_test.cpp`：Flux 文件执行、pipeline 行为。
- 新增 connector 单测：SQLite query、类型映射、错误处理、SQL builder。
- `flux_cli_unit_test.cpp`：CLI 输出和 examples 兼容。

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
- `_time` 在 SQL 中第一版约定为 RFC3339 text，还是支持更多数据库原生时间类型？
- `TableValue` 内嵌 plan 是否会让值模型过重，未来是否需要拆出 `TableStreamValue`？
- MySQL integration test 在当前 Bazel/CI 环境中如何稳定运行？
- 是否需要 `explain()` builtin 或 CLI 参数来显示 logical/physical plan？

## Recommended Next Step

下一轮建议继续补 provider 形态下的跨源场景和更多 pushdown 边界测试。

这样可以尽快验证“外部数据源进入 Flux”的闭环，同时不急着改变现有执行器主干。
