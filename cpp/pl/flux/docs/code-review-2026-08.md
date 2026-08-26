# Flux 代码审查报告（2026-08）

对 `cpp/pl/flux` 的一次全面代码审查结果沉淀。审查范围：`syntax`、`analysis`、`plan`、`optimizer`、`execution`、`runtime`、`connector`、`cli`、`contrib/lsp`。

审查时基线：`bazel test //cpp/pl/flux/...` 25 个测试全部通过。下文标注「已核实」的条目均在审查时逐条对照源码确认。

---

## 一、确认的 Bug（优先修复）

### 1. `token.h:185` — `LBrace` 字符串化错误（已核实）

```cpp
case TokenType::LBrace:
    return "LBrack";   // 应为 "LBrace"
```

错误消息和 AST dump 会把 `{` 显示成 `[`，复制粘贴失误。

### 2. `parser.cpp:1064` — `new_string_literal` 失败返回 nullptr（已核实）

`StrConv::parse_string` 失败（如非法 `\x` 转义）时静默返回 `nullptr`，`parse_label_literal`（`parser.cpp:1877`）随即 `->value` 解引用会崩溃；import 路径、属性 key 同样有风险。违反 CLAUDE.md 5.4「不允许静默吞错」。

### 3. `parser.cpp:86` — `parse_single_package` 空指针解引用（已核实）

缺少 package 声明的源文件使 `ast_file->package` 为 `nullptr`，直接 `->name` 崩溃。

### 4. `strconv.cpp:166` — `parse_time` 只支持 `Z` 结尾（已核实）

scanner 的 `time_offset` 规则接受 `+HH:MM`/`-HH:MM`，但 `parse_time` 硬编码 `%Y-%m-%dT%H:%M:%SZ`。合法的 `2024-01-01T00:00:00+08:00` 解析失败，scanner 与 strconv 语义不一致。

### 5. `runtime_value.h:57` — `TimeValue::operator<=>` 是字符串比较（已核实）

```cpp
struct TimeValue {
    std::string literal;
    auto operator<=>(const TimeValue&) const = default;  // 按 literal 字符串比较
};
```

同样问题存在于 `runtime_builtin_table_helpers.h:700`（sort 的 `compare_values`）和 `runtime_builtin_time_helpers.h:71`（解析失败静默回退字符串比较）。违反 CLAUDE.md 3.2「时间语义不能退回字符串近似」——带不同时区偏移的等价时间会给出错误的排序/比较结果。

### 6. `strconv.cpp:200` — `parse_magnitude` 无溢出检查（已核实）

手写累乘 `value = value * 10 + digit` 无溢出检测，超大 duration 字面量触发有符号溢出 UB。同文件 `parse_int_literal` 用 `std::stol` + catch，此处无保护。

### 7. `parser.h:253` — `depth_guard` 超深时计数器不回落

`depth_ > MAX_DEPTH` 提前 return 但未 `--depth_`，计数器永久偏高，嵌套深度限制比预期更早触发。

### 8. `mysql_source.cpp:424` — `quote_table_identifier` off-by-one（已核实）

`while (start <= identifier.size())` 在表名以 `.` 结尾时回退成整体加反引号（`` `db.` ``），生成非法 SQL。

### 9. `scheduler.cpp:302-403` — pipeline 依赖只校验不执行

拓扑排序验证无环后，所有 `DriverTask` 一次性并行提交，`dependencies` 字段是死代码。当前因 join 合并进单 pipeline 而「碰巧安全」，一旦拆成 build/probe 双 pipeline，probe 侧会在 hash table 构建完成前读取——数据竞争。建议强制依赖屏障，或删除该字段避免误导。

---

## 二、语义正确性疑点（需补测试确认）

### 10. `fill`/`elapsed`/`difference`/`derivative` 可能破坏逻辑表边界

`runtime_builtin_universe_transform.cpp:1164-1198`：`builtin_fill` 只遍历 `table->rows`，完全忽略 `tables`（多 chunk）。`TableValue`（`runtime_value.h:194-207`）同时有 `rows` 和 `tables` 两个字段，上游 `group()` 之后的多逻辑表流经过这些算子会被压扁/丢失。疑似违反 CLAUDE.md 3.1。四个算子共用 `materialized_table_ref` 后只读 `rows` 的模式。**先补「group 后接 fill」的回归测试确认行为。**

### 11. pipe 优先级与上游 Flux 规范不同

当前实现中 `|>` 绑定比 `+` 更紧（`exponent → pipe → unary` 链），`a |> f() + 1` 解析为 `(a |> f()) + 1`；官方 Flux 中 pipe 是最低优先级。两条解析路径（`parse_expression` 与 `parse_expression_suffix`）内部自洽，但与上游语义有偏差。需明确决策并写入 `docs/support-matrix.md`。

---

## 三、性能问题（修复前需先补 benchmark，见 CLAUDE.md 4.1）

| 位置 | 问题 |
|---|---|
| `runtime_eval.cpp:811`（已核实） | `=~` 每次匹配都现编译 `std::regex`，filter 热点路径重复编译 |
| `runtime_builtin_universe_transform.cpp:547` | `filter`/`map` 每行双重 `clone_row` |
| `runtime_value.cpp:383` | `ObjectValue::lookup` 线性扫描，pivot/join/group 反复调用 |
| `runtime_builtin_table_helpers.h:322` | `object_with_upserted_property` 每次全量拷贝 properties vector |
| `runtime_builtin_universe_join.cpp:229` | `join_rows` 用 `any_of` 去重，每行 O(K²) |
| `mysql_source.cpp:879` | `Statistics()` 每列 2 条串行 SQL，宽表性能灾难 |
| `analysis/builtin_metadata.cpp:589` | `FindBuiltinSignature` 线性扫描 80+ signature，LSP 补全高频调用 |

---

## 四、架构与工程问题

- **CBO 重复执行**：同一 logical plan 的 CBO 最多跑 3 次（`physical_planner.cpp:3255/3261`、`BuildOperator`、`explain.cpp:42`），且 CBO 内部查远端 metadata。
- **两套内存管控并存**：`QueryMemoryContext` 只覆盖 accumulator；page 流与 `ExchangeBuffer`（硬编码 64MB 背压）不受其管控；`page_budget.h` 只用于统计。内存超限时 `Release` 与增量 `Reserve` 不匹配，泄漏配额（`accumulator.cpp:126-158`）。
- **RBO Rule 接口被滥用**：8 条规则中 7 条只「打标签」不改写计划却返回 `applied=true`，trace 有误导性（`rbo.cpp:692`）。唯一真正改写的是 `InsertMaterializationBarrierRule`。
- **MySQL 非参数化 SQL 拼接**：`Scan()` 走 `BuildScanSql` 字符串拼接（`mysql_source.cpp:994`），应统一走参数化。
- **连接池静默降级**：DSN 解析失败 `impl_` 为 nullptr 且无告警（`mysql_connection_pool.cpp:126`）；`MySQLPageSource::Initialize()` 总开直连绕过池（ASAN workaround，`mysql_source.cpp:1305`）。
- **小毛病**：
  - `ast.h:674` 头文件 `static` 非 const 全局 map（每 TU 一份副本，应改 `inline const`）
  - `parser.h:54` 元数据写死 `parser-type=rust`，与 C++ 实现不符
  - `parser.h:147` `parse_regexp_literrer` 拼写错误
  - `mysql_error_message` 在 `mysql_connection_pool.cpp:45` 与 `mysql_source.cpp:326` 重复定义
  - `plan_node.h:235-276` typed accessor 不做 kind 检查，wrong-variant 调用直接 `std::terminate`，建议 debug 构建加断言
- **文档失准**：CLAUDE.md 的验证命令 `bazel test //cpp/pl/flux:all` 是空操作（0 个 target，已实测），应为 `bazel test //cpp/pl/flux/...`。

---

## 五、测试缺口

1. **`execution/` 整个目录无单测**：`PhysicalPlanner`、`PhysicalExecutor`、`Scheduler`、`Driver`、`TaskExecutor`、各 Streaming operator、`Materializer`、`QueryMemoryContext`、`PageBudget`、`ExchangeBuffer` 全裸奔，仅靠 `runtime_exec_unit_test.cpp` 端到端间接覆盖。优先补：`QueryMemoryContext`（Reserve/Release/超限边界）、`TaskExecutor`（Submit/Shutdown 语义）、`Scheduler`（空 task/cycle/多 pipeline）、`ExchangeBuffer`（并发与背压）。
2. **MySQL 连接器 CI 零覆盖**：全部测试依赖 `FLUX_MYSQL_TEST_DSN`，CI 100% SKIP。建议 testcontainer 或 docker compose MySQL 门禁。
3. `sql_builder` 仅 1 个测试用例，缺谓词/聚合/distinct/limit 场景。
4. 缺：时区偏移时间解析、`depth_guard` 深度保护、非法字符串转义、window/aggregateWindow 多 chunk + 日历 duration、CBO join distribution 选择的测试。

---

## 六、迭代建议（按性价比排序）

| 优先级 | 事项 | 关联条目 | 工作量 |
|---|---|---|---|
| P0 | 修确认的小 bug，每个配回归测试 | #1-#8 | 小 |
| P0 | 修 CLAUDE.md 测试命令；补 `execution/` 核心组件单测 | 五.1 | 中 |
| P1 | 补「group 后接 fill/elapsed/difference」回归测试，确认/修复逻辑表边界 | #10 | 小 |
| P1 | Scheduler 依赖屏障或删除 `dependencies` 字段 | #9 | 中 |
| P2 | 时间语义统一：`TimeValue` 改为解析后时间点比较 | #5 | 中大（动值模型） |
| P2 | 正则编译缓存 + `ObjectValue::lookup` 索引化（先补 benchmark） | 三 | 中 |
| P3 | MySQL 测试基建（testcontainer）+ 参数化 SQL 统一 | 四、五.2 | 大 |
