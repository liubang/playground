# Flux 代码审查报告（2026-08）

对 `cpp/pl/flux` 的一次全面代码审查结果沉淀。审查范围：`syntax`、`analysis`、`plan`、`optimizer`、`execution`、`runtime`、`connector`、`cli`、`contrib/lsp`。

审查时基线：`bazel test //cpp/pl/flux/...` 25 个测试全部通过。下文标注「已核实」的条目均在审查时逐条对照源码确认。

---

## 处理进展（2026-08-26）

**第一轮（commit `3d4dde215`）**：修复 #1-#7、#10 及第四节全部「小毛病」与文档失准；regex 编译缓存、`join_rows` 去重、`FindBuiltinSignature` 索引；#8、#9 复核判定为非 bug（理由见各条目）；#11 已写入 `docs/support-matrix.md`。补回归测试 14 个，端到端（CLI + docker MySQL 8.3）验收通过，25/25 测试全绿。

**第二轮**：修复 accumulator 内存配额 Reserve/Release 不匹配；连接池 `Acquire` 增加 30s 超时（认证失败从永久挂起变为有界报错）；`filter` 每行双 `clone_row` 消除（`linear:500000` 同机口径 median 0.518s → 0.455s，约 -12%）；MySQL `Statistics()` 列聚合 SQL 由每列一条合并为单条（保留逐列回退）；补 `execution/` 单测 16 个（`QueryMemoryContext`/`TaskExecutor`/`Scheduler`/`ExchangeBuffer`）；`sql_builder` 测试 1 → 10 个。

**第三轮**：RBO 接口拆分（检测规则上报 `detected` 而非 `applied`，explain 输出新增 `RBO(detected=[...])` 与 physical `detected:` 列表）；CBO 双跑合并（`Plan()` 按输入 plan 是否含 join 直接选择 Default/Fast 优化器，join 查询 CBO 从 2 次降为 1 次）；MySQL `Scan()` 统一走 `BuildParameterizedScanSql` + server-side prepared statement；补 `Materializer` 单测 4 个与 `aggregateWindow(1mo)` 多 chunk + 日历月边界回归测试。`ObjectValue::lookup` 索引化评估后**未合入**（benchmark 证据不足且 lazy mutable 状态的并发前提未证实）。

**第四轮（2026-08-27）**：连接池 DSN 解析失败两处静默降级改为显式告警（构造时 stderr + `Acquire` 携带原始解析错误，`MakeMySQLConnectorRuntime` 回退直连时告警）；`EstimateRowsForPlan` 增加按 `Plan()` 调用生命周期的 thread_local 记忆化缓存（聚合策略/分区检查不再重复跑完整 Default CBO）；消除 upsert 路径的二次深拷贝——新增 `Value::as_object_ptr()`，各 builtin 在构造 upsert 结果后共享不可变 `ObjectValue` 而非再拷一份（`group`/`map`/`set`/`duplicate`/`fill`/`elapsed`/`difference`/`derivative`/`window`/`quantile`/`reduce`/`join as`/`chunk_group_object`/`derive_chunk_group_key`，共享前提已全库核实：除 pivot 内部自建对象外无任何发布后原地修改）；补 `PhysicalPlanner`/`PhysicalExecutor` 端到端测试 4 个（scan+filter+limit、range+project streaming、join CBO distribution、两阶段 grouped aggregate）与 csv 大表 join/pivot/window 压力回归测试 2 个。基准（同机 median）：`group:1M` 1.913s → 1.530s（-20%），`agg:1M` 2.374s → 1.868s（-21%），`window:1M` -6.5%，`linear:1M` -2.4%。26/26 测试全绿。

**P0 崩溃专项复核（第四轮）**：在 HEAD 上未能复现。已执行的复现手段：`MallocNanoZone=0 MallocScribble=1 MallocPreScribble=1` 下完整 benchmark 矩阵（11 case × 3 规模 + 3 join 形态 × 2 规模，全绿）；join/pivot/window 手工组合查询 8 种 × 10-30 次；50k×50k grouped full join + pivot × 30 次；随机管道模糊测试 3 种子 × 300 轮 + 2 种子 × 400 轮（共 1700 轮，覆盖 filter/map/group/sort/limit/tail/distinct/unique/count/sum/mean/top/bottom/window/aggregateWindow/elapsed/difference/derivative/fill/duplicate/rename/drop/keep/set/range/pivot/join(inner/left/right/full)/union 随机组合，0 崩溃）；全部 examples × 3；ASan 严格模式（`detect_stack_use_after_return=1:detect_container_overflow=1`）下同批查询；sqlite lazy 物化 + csv join 组合。对 `runtime` builtin、`runtime_eval`、`runtime_page` 转换、`scheduler`/`physical_executor`、`accumulator`、CLI 输出路径的白盒审计未发现确定性悬垂/双重析构点。结论：保留「未定位」状态，但已固化两块回归网——`RuntimeExecTest.StressLargeGroupedFullJoinThenPivot`/`StressAggregateWindowThenPivot`（ASan 开启的默认 test 配置下每轮 CI 执行）与 mysql 集成的 CI 门禁。若再次捕获栈，优先检查：`SlicePageChunkRows` 在 `row_count > values.size()` 非法 chunk 下的 capacity 内越界读（对 ASan 不可见、对 MallocScribble 敏感，与已捕获栈特征最吻合）。

**新发现（未定位，专项处理）**：csv 大表 `join`/`pivot` 查询在 release build 下存在**间歇性崩溃**（SIGABRT，stderr 为空）。已捕获一次确定性栈：`malloc: pointer being freed was not allocated`，位于主线程嵌套函数调用深处 `Value`(variant) 析构 → `shared_ptr<ObjectValue>` 引用归零 → `~ObjectValue()` 释放非法指针，指向 `Value`/`ObjectValue` 的悬垂或双重析构。特征：`MallocScribble=1` 时窗口期 100% 复现，窗口外 0%；ASan/TSan/lldb 环境下不触发；与前三轮改动无关（同窗口逐版本对比证实，最早可复现版本未确定）。排查手段记录：`MallocScribble=1` + lldb `malloc_error_break` 断点。建议：启用 core dump 专项分析 lazy 表物化多 pipeline 路径的对象生命周期。

**Lazy 计划不可执行专项（第四轮追加，2026-08-28）**：排查「大数据量 sqlite 下 `range |> keep |> sort |> limit |> group` lazy 计划报 `plan is not executable`」。结论：在第四轮改动前后的代码（048947acf 与 HEAD）上，配合标准 1M 行 benchmark sqlite 数据均**无法复现**；问题与当时特定数据库文件的统计信息强相关，该文件已丢失。改进：`BuildOperator` 在拒绝计划时输出节点类型与输入数（原为裸 `plan is not executable`），并支持 `FLUX_DEBUG_PLAN=1` 环境变量输出完整 RBO 优化后计划结构，便于下次捕获时直接定位。26/26 测试保持全绿。

各条目状态：✅ 已修复 ｜ ❌ 复核不成立 ｜ ⏳ 待处理。

---

## 一、确认的 Bug（优先修复）

### 1. `token.h:185` — `LBrace` 字符串化错误（已核实）✅

```cpp
case TokenType::LBrace:
    return "LBrack";   // 应为 "LBrace"
```

错误消息和 AST dump 会把 `{` 显示成 `[`，复制粘贴失误。

### 2. `parser.cpp:1064` — `new_string_literal` 失败返回 nullptr（已核实）✅

`StrConv::parse_string` 失败（如非法 `\x` 转义）时静默返回 `nullptr`，`parse_label_literal`（`parser.cpp:1877`）随即 `->value` 解引用会崩溃；import 路径、属性 key 同样有风险。违反 CLAUDE.md 5.4「不允许静默吞错」。

### 3. `parser.cpp:86` — `parse_single_package` 空指针解引用（已核实）✅

缺少 package 声明的源文件使 `ast_file->package` 为 `nullptr`，直接 `->name` 崩溃。

### 4. `strconv.cpp:166` — `parse_time` 只支持 `Z` 结尾（已核实）✅

scanner 的 `time_offset` 规则接受 `+HH:MM`/`-HH:MM`，但 `parse_time` 硬编码 `%Y-%m-%dT%H:%M:%SZ`。合法的 `2024-01-01T00:00:00+08:00` 解析失败，scanner 与 strconv 语义不一致。

### 5. `runtime_value.h:57` — `TimeValue::operator<=>` 是字符串比较（已核实）✅

```cpp
struct TimeValue {
    std::string literal;
    auto operator<=>(const TimeValue&) const = default;  // 按 literal 字符串比较
};
```

同样问题存在于 `runtime_builtin_table_helpers.h:700`（sort 的 `compare_values`）和 `runtime_builtin_time_helpers.h:71`（解析失败静默回退字符串比较）。违反 CLAUDE.md 3.2「时间语义不能退回字符串近似」——带不同时区偏移的等价时间会给出错误的排序/比较结果。

### 6. `strconv.cpp:200` — `parse_magnitude` 无溢出检查（已核实）✅

手写累乘 `value = value * 10 + digit` 无溢出检测，超大 duration 字面量触发有符号溢出 UB。同文件 `parse_int_literal` 用 `std::stol` + catch，此处无保护。

### 7. `parser.h:253` — `depth_guard` 超深时计数器不回落 ✅

`depth_ > MAX_DEPTH` 提前 return 但未 `--depth_`，计数器永久偏高，嵌套深度限制比预期更早触发。

### 8. `mysql_source.cpp:424` — `quote_table_identifier` off-by-one（已核实）❌ 复核不成立

复核结论：表名以 `.` 结尾时回退为整体加反引号（`` `db.` ``），这是对非法输入的防御性回退，产生的是合法引用标识符而非非法 SQL；改成 `start < size` 反而会静默丢弃尾点。维持现状。

`while (start <= identifier.size())` 在表名以 `.` 结尾时回退成整体加反引号（`` `db.` ``），生成非法 SQL。

### 9. `scheduler.cpp:302-403` — pipeline 依赖只校验不执行 ❌ 复核不成立

复核结论：跨 pipeline 数据流全部经过 `ExchangeBuffer`（阻塞读 + 背压），join 的 build/probe 在消费侧 pipeline 内串行完成，不存在审查假设的竞争窗口；若强制「依赖完成才启动」反而会与背压死锁。`dependencies` 用于成环/缺失校验与 profile 展示，已在 `physical_executor.h` 注释中澄清语义。

拓扑排序验证无环后，所有 `DriverTask` 一次性并行提交，`dependencies` 字段是死代码。当前因 join 合并进单 pipeline 而「碰巧安全」，一旦拆成 build/probe 双 pipeline，probe 侧会在 hash table 构建完成前读取——数据竞争。建议强制依赖屏障，或删除该字段避免误导。

---

## 二、语义正确性疑点（需补测试确认）

### 10. `fill`/`elapsed`/`difference`/`derivative` 可能破坏逻辑表边界 ✅

已确认并修复：四个算子全部改为按 chunk 迭代、逐 chunk 输出，per-group 状态不跨 chunk 泄漏；补了「group 后接 fill/elapsed/difference/derivative」回归测试。

`runtime_builtin_universe_transform.cpp:1164-1198`：`builtin_fill` 只遍历 `table->rows`，完全忽略 `tables`（多 chunk）。`TableValue`（`runtime_value.h:194-207`）同时有 `rows` 和 `tables` 两个字段，上游 `group()` 之后的多逻辑表流经过这些算子会被压扁/丢失。疑似违反 CLAUDE.md 3.1。四个算子共用 `materialized_table_ref` 后只读 `rows` 的模式。**先补「group 后接 fill」的回归测试确认行为。**

### 11. pipe 优先级与上游 Flux 规范不同 ✅

决策：保持现有解析行为不变（两条解析路径内部自洽，变更成本高），偏差已写入 `docs/support-matrix.md` 的 pipe 表达式条目。

当前实现中 `|>` 绑定比 `+` 更紧（`exponent → pipe → unary` 链），`a |> f() + 1` 解析为 `(a |> f()) + 1`；官方 Flux 中 pipe 是最低优先级。两条解析路径（`parse_expression` 与 `parse_expression_suffix`）内部自洽，但与上游语义有偏差。需明确决策并写入 `docs/support-matrix.md`。

---

## 三、性能问题（修复前需先补 benchmark，见 CLAUDE.md 4.1）

| 位置 | 问题 | 状态 |
|---|---|---|
| `runtime_eval.cpp:811`（已核实） | `=~` 每次匹配都现编译 `std::regex`，filter 热点路径重复编译 | ✅ 已加编译缓存 |
| `runtime_builtin_universe_transform.cpp:547` | `filter`/`map` 每行双重 `clone_row` | ✅ filter 已消除（median -12%）；map 单次 clone 不可避免 |
| `runtime_value.cpp:383` | `ObjectValue::lookup` 线性扫描，pivot/join/group 反复调用 | ⏳ 已评估未合入（第三轮）：阈值式惰性索引已实现但 benchmark 证据不足、lazy mutable 状态并发前提未证实，回滚保留结论 |
| `runtime_builtin_table_helpers.h:322` | `object_with_upserted_property` 每次全量拷贝 properties vector | ✅ 第四轮：消除各调用点的二次深拷贝（`as_object_ptr()` 共享不可变对象，`group:1M` -20%、`agg:1M` -21%）；不可变行模型本身保留 |
| `runtime_builtin_universe_join.cpp:229` | `join_rows` 用 `any_of` 去重，每行 O(K²) | ✅ 已改哈希集合 |
| `mysql_source.cpp:879` | `Statistics()` 每列 2 条串行 SQL，宽表性能灾难 | ✅ 列聚合已合并为单条 SQL（MCV 保留逐列） |
| `analysis/builtin_metadata.cpp:589` | `FindBuiltinSignature` 线性扫描 80+ signature，LSP 补全高频调用 | ✅ 已加哈希索引 |

---

## 四、架构与工程问题

- **CBO 重复执行** ⏳（部分修复）：~~`physical_planner.cpp:3255/3261` join 查询 CBO 双跑~~ ✅ 第三轮已合并为单次（按输入 plan 是否含 join 直接选择 Default/Fast）。~~`EstimateRowsForPlan` 每次完整 Default CBO~~ ✅ 第四轮已加按 `Plan()` 调用生命周期的 thread_local 记忆化缓存。残余：`BuildOperator` 内嵌套子计划的 Fast CBO、`explain.cpp` 每节点一次 Default CBO（诊断路径，改造需保持输出语义）。
- **两套内存管控并存** ⏳（部分修复）：`QueryMemoryContext` 只覆盖 accumulator；page 流与 `ExchangeBuffer`（硬编码 64MB 背压）不受其管控；`page_budget.h` 只用于统计。~~内存超限时 `Release` 与增量 `Reserve` 不匹配，泄漏配额（`accumulator.cpp:126-158`）~~ ✅ 第二轮已修复：`QueryMemoryContext::Reserve` 失败回滚 `used_bytes_`，`account_accumulator_memory` 仅在 Reserve 成功后计数，Release 与成功 Reserve 严格配对（含回归测试）。
- **RBO Rule 接口被滥用** ✅：第三轮已拆分——`RuleApplication`/`RuleTrace` 新增 `detected` 字段，7 条检测规则改为上报 `detected`；`AppliedRuleNames` 只含真实改写（`InsertMaterializationBarrierRule`），explain 文本与 physical trace 分别展示 `rules` 与 `detected`。
- **MySQL 非参数化 SQL 拼接** ✅：第三轮已统一——`Scan()` 改走 `BuildParameterizedScanSql` + server-side prepared statement（新增 `execute_prepared` helper，与 split page source 同构），docker MySQL 8.3 集成测试验证。
- **连接池静默降级** ⏳（部分修复）：~~DSN 解析失败 `impl_` 为 nullptr 且无告警（`mysql_connection_pool.cpp:126`）~~ ✅ 第四轮已修复：构造时 stderr 告警并记录 `init_error_`，`Acquire` 错误携带原始解析错误；`MakeMySQLConnectorRuntime` 回退直连时也告警（含非 DSN-gated 回归测试）。残余：`MySQLPageSource::Initialize()` 总开直连绕过池（ASAN workaround，`mysql_source.cpp:1305`，上游 Boost.MySQL container-overflow 未修复前保留）。✅ 第二轮已修复关联问题：E2E 实测认证失败时 `async_get_connection` 无限重试导致查询永久挂起，`Acquire` 已增加 30s 超时（默认，可覆盖），返回 `DeadlineExceeded`；含 DSN-gated 回归测试。
- **小毛病**（✅ 第一轮全部修复）：
  - `ast.h:674` 头文件 `static` 非 const 全局 map（每 TU 一份副本，应改 `inline const`）
  - `parser.h:54` 元数据写死 `parser-type=rust`，与 C++ 实现不符
  - `parser.h:147` `parse_regexp_literrer` 拼写错误
  - `mysql_error_message` 在 `mysql_connection_pool.cpp:45` 与 `mysql_source.cpp:326` 重复定义
  - `plan_node.h:235-276` typed accessor 不做 kind 检查，wrong-variant 调用直接 `std::terminate`，建议 debug 构建加断言
- **文档失准** ✅：CLAUDE.md 的验证命令 `bazel test //cpp/pl/flux:all` 是空操作（0 个 target，已实测），应为 `bazel test //cpp/pl/flux/...`。

---

## 五、测试缺口

1. **`execution/` 整个目录无单测** ⏳（部分补齐）：第二轮已新增 `execution/tests/execution_unit_test.cpp`（16 个用例），覆盖 `QueryMemoryContext`（Reserve/Release/超限回滚）、`TaskExecutor`（Submit/Shutdown/并发）、`Scheduler`（空 task/缺失依赖/cycle/单 pipeline 运行/算子失败传播）、`ExchangeBuffer`（FIFO/背压阻塞/多生产者 Finish/MarkError/Close/线程间传递）；第三轮补 `Materializer` 4 个用例（passthrough/无 plan 拒绝/经 plan 物化并保留 plan/range/result 字段）；第四轮补 `PhysicalPlanner`/`PhysicalExecutor` 端到端 4 个用例（scan+filter+limit、range+project streaming operator、join CBO distribution、两阶段 grouped aggregate，均走 memory connector 真实执行）。`PageBudget` 仍仅靠端到端间接覆盖。
2. **MySQL 连接器 CI 零覆盖** ✅ 复核不成立（第四轮）：`.github/workflows/build_cpp.yml` 的 `test-linux-clang` 与 `coverage-linux-gcc` 均已配置 `mysql:8.4` service、导入 `mysql_metrics.sql` fixture 并以 `--test_env=FLUX_MYSQL_TEST_DSN` 运行，DSN-gated 测试在 Linux CI 真实执行；仅 macOS job 无 MySQL（runner 限制，可接受）。
3. `sql_builder` 仅 1 个测试用例，缺谓词/聚合/distinct/limit 场景。✅ 第二轮已补至 10 个用例（时间范围/谓词/聚合/distinct/投影别名/排序/limit-offset/参数绑定顺序与 normalize_time 标记/contract 拒绝路径）。
4. 缺：~~时区偏移时间解析、`depth_guard` 深度保护、非法字符串转义~~（✅ 第一轮已补）、~~window/aggregateWindow 多 chunk + 日历 duration~~（✅ 第三轮已补：group 后 `aggregateWindow(1mo)` 双 group 双日历月 chunk 结构与边界断言）、~~CBO join distribution 选择的测试~~（✅ 第四轮已补：`PhysicalPlannerTest.ExecutesJoinWithCboDistributionChoice` 断言 distribution 无关的结果正确性）。

---

## 六、迭代建议（按性价比排序）

| 优先级 | 事项 | 关联条目 | 工作量 | 状态 |
|---|---|---|---|---|
| P0 | 修确认的小 bug，每个配回归测试 | #1-#8 | 小 | ✅ 第一轮（#8 复核不成立） |
| P0 | 修 CLAUDE.md 测试命令；补 `execution/` 核心组件单测 | 五.1 | 中 | ✅ 第一轮 + 第二轮（余 PhysicalPlanner/Streaming operator/Materializer） |
| P1 | 补「group 后接 fill/elapsed/difference」回归测试，确认/修复逻辑表边界 | #10 | 小 | ✅ 第一轮 |
| P1 | Scheduler 依赖屏障或删除 `dependencies` 字段 | #9 | 中 | ❌ 复核不成立（已注释澄清） |
| P2 | 时间语义统一：`TimeValue` 改为解析后时间点比较 | #5 | 中大（动值模型） | ✅ 第一轮 |
| P2 | 正则编译缓存 + `ObjectValue::lookup` 索引化（先补 benchmark） | 三 | 中 | 正则缓存 ✅ 第一轮；lookup 索引化 ⏳ |
| P3 | MySQL 测试基建（testcontainer）+ 参数化 SQL 统一 | 四、五.2 | 大 | ✅ 第三轮（参数化 SQL）+ 第四轮复核（CI 已有 mysql service 门禁，无需 testcontainer） |
| P3 | `ObjectValue::lookup` 索引化、`upsert` 全量拷贝、CBO 双跑合并、RBO 接口拆分、window 多 chunk 日历 duration 测试 | 三、四、五 | 中大 | 部分 ✅ 第三轮（CBO 双跑/RBO/测试）+ 第四轮（upsert 二次拷贝消除）；lookup 已评估未合入 |
| P0 | **专项**：定位 csv 大表 join/pivot 间歇性崩溃（release SIGABRT，Value/ObjectValue 悬垂或双重析构） | 进展.新发现 | 中 | ⏳ 第四轮专项复核未复现（约 1700 轮模糊测试 + 全量 benchmark + ASan 严格模式，详见进展节）；已固化压力回归测试进 CI，保留观察 |
