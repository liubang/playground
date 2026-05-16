# Flux 解析器支持矩阵

本文档描述 `cpp/pl/flux` 当前的实现状态。

状态说明：

- `支持`：已经实现，并且有现有测试或调试工具覆盖
- `部分支持`：常见路径可用，但实现还不完整，或边界情况仍然脆弱
- `缺失`：尚未实现，或还不够稳定，不能宣称已支持

## 文件级语法

| 语法                     | 状态 | 说明                                                                                          | 示例                     |
| ------------------------ | ---- | --------------------------------------------------------------------------------------------- | ------------------------ |
| `package` 子句           | 支持 | 解析为 `PackageClause`                                                                        | `package metrics`        |
| `import "path"`          | 支持 | 无别名导入可用                                                                                | `import "array"`         |
| `import alias "path"`    | 支持 | 别名会保存在 AST 中                                                                           | `import regexp "regexp"` |
| 文件体语句列表           | 支持 | 支持混合语句列表                                                                              | `a = 1`                  |
| attributes / annotations | 支持 | package/import/statement 上的属性都会挂到 AST，block statement 也支持，属性参数支持完整表达式 | `@edition("2022.1")`     |

## 语句

| 语法                  | 状态     | 说明                                 | 示例                                     |
| --------------------- | -------- | ------------------------------------ | ---------------------------------------- |
| 表达式语句            | 支持     | 可从顶层表达式解析                   | `from(bucket: "x")`                      |
| 变量赋值              | 支持     | 主赋值路径可用                       | `status = "ok"`                          |
| `option name = expr`  | 支持     | 解析为带变量赋值的 `OptionStatement` | `option task = {name: "cpu"}`            |
| `option a.b = expr`   | 支持     | 成员赋值路径可用                     | `option task.offset = 5m`                |
| `return expr`         | 支持     | 可在 block 中使用                    | `return 1 + 2`                           |
| `builtin name : type` | 支持     | 支持函数类型和 record 类型           | `builtin sum : (a: int) => int`          |
| `testcase` 语句       | 支持     | 基础 `extends` 与 block 解析可用     | `testcase t extends "base" { return 1 }` |
| 非法语句恢复          | 部分支持 | 存在 `BadStmt`，但恢复粒度仍较粗     | 非法输入                                 |

## 表达式

| 语法             | 状态 | 说明                                                                       | 示例                               |
| ---------------- | ---- | -------------------------------------------------------------------------- | ---------------------------------- |
| 标识符           | 支持 | `true` / `false` 会特判为布尔字面量                                        | `value`                            |
| 整数字面量       | 支持 | 基本整数解析可用                                                           | `123`                              |
| 浮点字面量       | 支持 | 常见十进制形式可用                                                         | `0.5`                              |
| 字符串字面量     | 支持 | 基础字符串解析可用                                                         | `"cpu"`                            |
| 字符串插值       | 支持 | 支持 `${expr}`                                                             | `"host ${user}"`                   |
| duration 字面量  | 支持 | 常见单位可解析                                                             | `1h`, `5m`                         |
| datetime 字面量  | 支持 | 当前实现支持 RFC3339 风格                                                  | `2024-01-02T03:04:05Z`             |
| 正则字面量       | 支持 | 已支持依赖表达式上下文的正则扫描                                           | `/cpu.*/`                          |
| 布尔字面量       | 支持 | `true` / `false` 映射为 `BooleanLit`                                       | `true`                             |
| 数组字面量       | 支持 | 常见数组形式可用                                                           | `["cpu", "mem"]`                   |
| 字典字面量       | 支持 | key/value 形式可用                                                         | `["cpu": 1, "mem": 2]`             |
| 对象字面量       | 支持 | 标准对象属性可用                                                           | `{name: "cpu"}`                    |
| record update    | 支持 | 支持 `with` 来源                                                           | `{base with enabled: true}`        |
| 成员访问         | 支持 | 解析为 `MemberExpr`                                                        | `config.enabled`                   |
| 索引访问         | 支持 | 数字索引可用；对象上的字符串索引会规范化为成员式访问                       | `arr[0]`, `obj["enabled"]`         |
| 一元表达式       | 支持 | 包含 `exists`、一元 `-`、`not`                                             | `exists config.enabled`            |
| 二元表达式       | 支持 | 常见算术和比较运算可用                                                     | `a + b`, `x =~ /cpu.*/`           |
| 逻辑表达式       | 支持 | `and` / `or` 在常见场景下可用                                              | `a and b`                          |
| 条件表达式       | 支持 | `if ... then ... else ...` 可用                                            | `if exists x then "ok" else "bad"` |
| 调用表达式       | 支持 | 常见调用语法可用                                                           | `range(start: -1h)`                |
| 函数表达式       | 支持 | 带括号参数、单参简写箭头、块体、pipe 参数、可选参数/默认参数都已有测试覆盖 | `(r) => r.host == "local"`         |
| pipe 表达式      | 支持 | 多段管道链可用                                                             | `from(...) \|> range(...)`         |
| label 字面量     | 支持 | 顶层 label 表达式已能走正常表达式语句路径                                  | `.field`                           |
| 括号表达式       | 支持 | 基础分组可用                                                               | `(1 + 2)`                          |
| 无符号整数字面量 | 支持 | `123u` 风格字面量可扫描并解析为 `UnsignedIntegerLit`                       | `42u`                              |

## 类型语法

| 语法                         | 状态 | 说明                                                 | 示例                                |
| ---------------------------- | ---- | ---------------------------------------------------- | ----------------------------------- |
| 基础类型                     | 支持 | `int`、`string`、`bool` 等命名类型可用               | `int`                               |
| 类型变量                     | 支持 | 单 token 类型变量可在类型表达式与约束中解析          | `A`                                 |
| 数组类型                     | 支持 | 支持 `[type]`                                        | `[int]`                             |
| 字典类型                     | 支持 | 支持 `[key:value]`                                   | `[string:int]`                      |
| record 类型                  | 支持 | 常见属性形式可用                                     | `{name: string, value: int}`        |
| 带 `with` 来源的 record 类型 | 支持 | 当前解析器支持这一基本形态                           | `{A with name: string}`             |
| 函数类型                     | 支持 | 必填、可选、pipe 参数在常见场景下可用                | `(<-tables: [int], ?n: int) => int` |
| dynamic 类型                 | 支持 | 可解析为 `Dynamic` monotype，并在 AST/调试输出中可见 | `dynamic`                           |
| vector / stream 类型         | 支持 | 解析为专门的 monotype，且包含 malformed 恢复测试     | `vector[int]`, `stream[int]`        |
| `where` 约束                 | 支持 | 单条和逗号分隔的基础约束都可解析                     | `where A: Addable`                  |

## 调试与工具

| 能力                 | 状态     | 说明                                                                                                                                                                                                                                               |
| -------------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| AST 树形 dump        | 支持     | `dump_ast(const File&)` 和 `flux ast` 默认输出                                                                                                                                                                                                     |
| AST JSON dump        | 支持     | `dump_ast_json(const File&)` 和 `flux ast --json`                                                                                                                                                                                                  |
| 命令行 AST dump 工具 | 支持     | `bazel build //cpp/pl/flux:flux` 后可用 `flux ast`                                                                                                                                                                                                 |
| 命令行运行时执行     | 部分支持 | `flux -e`、`flux path/to/query.flux` 和共享环境 REPL 可用；未实现的执行特性仍会在运行时报错                                                                                                                                                        |
| CLI 结果展示         | 部分支持 | 标量表达式仍输出紧凑值；查询脚本会输出命名结果块和逻辑表，已能显示多表信息和显式保留的空逻辑表，但与官方 Influx 结果集格式仍未完全对齐                                                                                                             |
| 结构化结果输出       | 部分支持 | CLI 支持 `--list-results`、`--output-format human \| csv \| json`、`--result <name>`；CSV 会按逻辑表输出注解块并复用 `result`/`table` 列，JSON 会暴露结果名、逻辑表列信息和 group key 元数据，但官方结果流保真度仍未完全补齐                         |
| 解析器演示二进制     | 支持     | `parser_test` 已使用树形 dump                                                                                                                                                                                                                      |
| 解析器单测           | 支持     | 覆盖主路径与 dump 输出                                                                                                                                                                                                                             |
| scanner 单测         | 支持     | 覆盖注释、正则模式和 unread 行为                                                                                                                                                                                                                   |
| AST 源码位置         | 支持     | 顶层 file/package/import/statement/expression/block 节点都保存位置并可 dump（`loc=1:1-1:10`）                                                                                                                                                      |

## 运行时基础

| 能力                            | 状态     | 说明                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ------------------------------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 运行时值模型                    | 支持     | 支持 null/bool/int/uint/float/string/duration/time/regex/array/object，以及内存内表值；表值现已支持一个 `TableValue` 承载多张逻辑表                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 词法环境                        | 支持     | 父作用域、变量绑定、option 绑定和最近作用域赋值都有单测覆盖                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 表达式求值器                    | 支持     | 支持字面量、标识符、数组/对象、record update、成员/索引访问、一元/二元/逻辑运算、条件表达式、字符串插值、正则匹配、函数值、函数调用，以及内存内 `array.from` / `csv.from` / `sqlite.from` / `range` / `filter` / `map` / `limit` / `tail` / `keep` / `drop` / `rename` / `duplicate` / `set` / `reduce` / `sort` / `group` / `window` / `pivot` / `fill` / `elapsed` / `difference` / `derivative` / `distinct` / `count` / `spread` / `quantile` / `median` / `first` / `last` / `top` / `bottom` / `union` / `join` / `aggregateWindow` 查询执行                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 语句执行                        | 支持     | 支持变量赋值、`option` 赋值、表达式语句、block/return、`testcase` 在隔离子作用域中执行、文件级顺序执行、顶层 `builtin` 声明、package/import 元数据处理，以及简单内存查询文件的端到端执行                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| 函数值 / 闭包                   | 支持     | 用户自定义函数表达式会求值为可调用的运行时值，并捕获词法闭包                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| 函数调用执行                    | 支持     | builtin 和用户函数都可调用，支持默认参数、命名参数、块体函数、pipe 参数注入，以及查询 builtin 内部的行函数调用                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| pipe 执行                       | 部分支持 | `\|>` 已支持 builtin、用户定义 `<-pipe` 参数以及内存表管道；`option task = {...}` 这类 option 绑定现在也可以直接驱动表达式与管道参数，但更广泛的 Flux 流式语义仍未完整实现                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| builtin 注册表 / stdlib 执行    | 部分支持 | 默认加载常用 universe 风格顶层 builtin，包括 scalar helper、table transform、selector/aggregate、`join()`、`aggregateWindow`、`yield` 和 `explain`；显式 package 覆盖 `array`、`csv`、`sqlite`、`mysql`、`date`、`dict`、`regexp`、`strings`、`math`、`json`、`runtime`、`system`、`types`、`join`。没有 universe 顶层数据源入口，数据源走 provider package API；顶层 `builtin` 声明可绑定已知 builtin 或占位 callable；未知 package 会绑定为 metadata-only object；完整 Flux 标准库仍远未实现。各数据源和重点 builtin 的具体能力见本表其他行 |
| stdlib package conformance 示例 | 支持     | `examples/stdlib_conformance` 为 `array`、`csv`、`date`、`dict`、`join`、`json`、`math`、`regexp`、`runtime`、`sqlite`、`strings`、`system`、`types` 和默认 universe builtin 提供固定输入/输出 `.flux` 样例；`//cpp/pl/flux:stdlib_conformance_test` 会用 JSON 快照校验这些契约，`system.time` 使用 RFC3339 形状匹配                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| `join()` / `join` package 语义  | 部分支持 | 默认加载的 universe 顶层 `join()` 支持旧版 `join(tables:, on:)` 形态、两个输入流和 `method: "inner" \| "left" \| "right" \| "full"`，只会比较 group key 实例相同的逻辑表，输出保持多表流，重复非 `on` 列会按 `<column>\_<table>` 重命名，`null`/ 缺失 join key 不匹配；显式 `import "join"` 绑定的是 package 对象，提供 `join.inner`/`join.left`/`join.right`/`join.full`，支持官方风格 predicate `on` 和 `as` 输出函数，也兼容 `on: ["col"]` 的列数组形式；更完整的官方 join 包边界仍需继续补齐                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `window()` 语义                 | 部分支持 | `window()` 已支持固定时长窗口、日历窗口、`period`、`offset`、`location`、`startColumn`、`stopColumn`、`timeColumn`、`createEmpty`，并会把输入拆成真正的多逻辑表输出；更广的官方 Flux 细节和与更多下游 builtin 的组合行为仍需要继续补齐                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| CSV 输入                        | 部分支持 | `import "csv"`、`csv.from(csv: ...)`、`csv.from(file: ...)` 支持 raw 模式和常见 annotated CSV；支持 `#datatype`、`#group`、可选 `#default`、类型转换，以及同一载荷内重复 metadata/header block，对应多逻辑表输入；更广的 CSV stdlib 能力尚未补齐                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| SQLite 输入                     | 部分支持 | `import "sqlite"`、`sqlite.from(path:, table:)` 可通过 SQLite C API 扫描表；用户 API 不提供 `query` 入口；支持 null/int/float/text/blob-as-string 类型映射；执行主路径走 connector metadata / split / page source 和 Page pipeline；支持 `range/filter(simple)/keep/drop/rename/sort/limit/distinct` 线性前缀下推，以及简单 `group(columns:) \|> count/sum/mean/min/max(column:)` 聚合；SQL pushdown 生成前会统一校验 projection/predicate/distinct/group/aggregate/sort/limit contract；scan/filter/project/range 支持 rowid multi-split，Top-N 支持 split 内 partial Top-N 加 root 全局 Top-N，blocking unary operator 和 root exchange 直接消费 Page，split profile 包含 rows/pages/bytes/wall time；复杂 `filter(fn:)` 和跨源 join 下推仍未实现 |
| MySQL 输入                      | 部分支持 | `import "mysql"`、`mysql.from(dsn:, table:)` 和 `mysql.from(host:, user:, password:, database:, table:, ?port:)` 可通过 Boost.MySQL 扫描表；用户 API 不提供 `query` 入口；支持 `mysql://user:password@host[:port]/database` 与 `user:password@tcp(host[:port])/database` DSN；支持 null、signed/unsigned integer、float/double、string/blob、date/datetime/time 映射；执行主路径走 connector metadata / split / page source 和 Page pipeline；支持 `range/filter(simple)/keep/drop/rename/sort/limit/distinct` 线性前缀下推，以及简单 `group(columns:) \|> count/sum/mean/min/max(column:)` 聚合；SQL pushdown 复用 SQLite 同一套 contract 校验；scan/filter/project/range 会基于主键或整型列做保守 range split，Top-N 支持 split 内 partial Top-N 加 root 全局 Top-N，blocking unary operator 和 root exchange 直接消费 Page，split profile 包含 rows/pages/bytes/wall time；复杂 `filter(fn:)` 和跨源 join 下推仍未实现 |
| aggregate windows               | 部分支持 | RFC3339 `_time` 上的固定时长窗口可用，支持 `column`、`offset`、`period`、`timeSrc`、`timeDst`、`mean` / `sum` / `min` / `max`、自定义数组函数、窗口 `count`、负 `period`、`every != period` 的重叠窗口、`range()` 边界上的 `createEmpty: true`；日历 `mo` / `y` 窗口支持显式固定偏移和命名时区 `location` 记录，并支持日历窗口 `offset`；selector 风格 `first` / `last` 会像官方 Flux 一样丢弃空窗口；窗口输出现在会按逻辑表与 group key 一起保真，避免把不同逻辑表里同名 group 意外合并；未显式传参时也会回退到全局 `option location`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| elapsed transforms              | 部分支持 | `elapsed(unit:, timeColumn:, columnName:)` 已支持固定时长单位和 RFC3339 `_time` 类列，会按逻辑表逐表丢掉首行并写入整数 elapsed 值；日历单位仍未实现                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| difference transforms           | 部分支持 | `difference(column:, nonNegative:, keepFirst:)` 已支持数值列，会在每张逻辑表内按行计算 delta，`nonNegative: true` 可将负增量置空，`keepFirst: true` 可保留首行并给出空 delta；更广泛 Flux 语义尚未补齐                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| derivative transforms           | 部分支持 | `derivative(unit:, column:, timeColumn:, nonNegative:, initialZero:)` 已支持数值列与 RFC3339 `_time` 类列，会在每张逻辑表内计算按单位归一化的浮点速率，支持 `nonNegative` 和 `initialZero`；日历单位与更多 Flux 语义仍未实现                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |

## 错误处理

| 能力                  | 状态     | 说明                                                                                                                                 |
| --------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| 解析错误收集          | 支持     | `Parser::errors()` 可暴露收集到的解析错误                                                                                            |
| 局部失败后继续解析    | 部分支持 | 在条件表达式、调用参数、数组、字典、对象属性、属性注解，以及 vector/stream/record type 的一部分 malformed 输入上已有恢复，但还不统一 |
| 精确语法诊断          | 部分支持 | 很多错误信息已经有用并带有局部上下文，但一致性仍有限                                                                                 |
| 带源码位置的 AST 错误 | 部分支持 | 很多坏表达式/坏语句都会携带位置，但还未覆盖全部解析错误                                                                              |

## 当前优势

- 常见 Flux 文件结构已经可以稳定解析为 AST
- 重管道查询形态已经能跑通
- 常见表检查辅助函数已经可用，包括 `columns`、`keys`、`findColumn`、`findRecord`
- 带 alias import、`filter` / `map` 链、正则调用、record update、条件表达式的真实查询片段已有解析测试覆盖
- 函数表达式已经覆盖大多数常见 Flux 查询形态：`(r) => expr`、`r => expr`、`r => { ... }`、`(<-tables, ?limit=5, value) => expr`
- 空字面量与空 record update 已覆盖，包括 `[]`、`{}`、`[:]`、`{base with}`
- 类型解析已经足以支撑 `builtin` 声明与 AST 调试
- AST 树形与 JSON 输出便于观察解析器行为
- CLI 已可执行代码片段、文件和交互式 REPL 输入
- 当前单测覆盖已经足够让解析器和运行时继续安全扩展
- 运行时具备可测试的值模型、作用域链、表达式求值器和首版语句执行器
- CSV 数据可以通过 Flux 风格的 `csv.from` 进入运行时，包括 raw 头行 CSV、annotated CSV，以及重复 metadata block 的多表输入
- 内联内存表统一通过 `import "array"` + `array.from(rows:)` 构造；运行时不提供顶层 `from`
- `group` 已实现真实的按 group key 重分表语义，并支持 `mode: "by"` / `mode: "except"`
- `count`、`first`、`last`、`distinct`、`sort`，以及 `filter` / `map` / `limit` / `tail` / `keep` / `drop` / `rename` / `duplicate` / `set` / `reduce` 等算子已经按逻辑表逐表工作，不再是旧的“全表 + `_group` 标签”行为
- `join()` 已经按相同 group key 实例配对逻辑表，不同 measurement / field 的 join 示例也会先显式 regroup 再连接

## 主要已知缺口

- 某些语法族仍然只是部分实现，或者测试覆盖较薄
- 非法程序的错误恢复仍不均衡
- 少数 AST/debug 字符串形式仍然是简化版，不是标准 Flux 格式
- 还没有语义分析或类型检查层
- 运行时执行仍是“可用子集”，虽然已经能跑常见内存查询管道，但更广的 builtin 和流式语义仍缺失
- `array.from` / `array.concat` / `array.filter` / `array.map` / `array.contains` / `array.reduce` / `array.any` / `array.all` / `csv.from` / `sqlite.from` / `mysql.from` / `date.*` / `dict.*` / `regexp.*` / `strings.*` / `math.*` / `json.encode` / `runtime.version` / `system.time` / `types.*` / `join.*` 这类 package 入口已可用，但完整标准库接口仍远未实现
- CLI 输出已经具备结果导向和多表感知能力，但距离官方 Influx 结果集格式仍有差距

## 推荐下一步

1. 继续补齐剩余的部分语法实现，并增加更真实的端到端查询 fixture
2. 继续改善 malformed 输入恢复与诊断，让坏程序也能产出尽量可用的 AST/debug 输出
3. 增加更多负例测试和 dump 快照
4. 把 `SourceLocation` 覆盖范围扩展到更多 AST 节点
5. 在现有执行架构上继续扩展 builtin、流语义和查询执行
6. 继续提升结果输出层，使 CLI 的 human / CSV / JSON 更接近官方 Flux / Influx 查询体验

## builtin / stdlib 拆分 TODO

当前目标是按官方 Flux 标准库边界重整 builtin：默认加载的查询函数归入 `universe` 心智模型，显式 `import "..."` 的能力按官方 package 路线扩展。C++ 文件可以继续按实现复杂度拆分，但用户可见的命名空间优先跟官方保持一致。拆分原则是先做机械迁移、保持行为不变；迁移稳定后，再在对应模块里补新的 package API。

数据源入口不归入默认 universe：当前使用 `array.from`、`csv.from`、`sqlite.from`、`mysql.from` 这类 provider package 形态扩展。

### 官方 package 对齐

- 已有：`array`，包含 `array.from`、`array.concat`、`array.filter`、`array.map`、`array.contains`、`array.reduce`、`array.any`、`array.all`
- 已有：`csv`，包含 `csv.from`
- 已有：`sqlite`，包含 `sqlite.from`；connector / plan / pushdown 见 `DATASOURCE_ARCHITECTURE.md`
- 已有：`mysql`，包含 `mysql.from(dsn:, table:)` 和显式连接字段形态；connector / plan / pushdown 复用 SQL provider 路线
- 已有：`date`，包含 `date.add`、`date.sub`、`date.truncate`、`date.year`、`date.month`、`date.monthDay`、`date.weekDay`、`date.hour`、`date.minute`、`date.second`
- 已有：`regexp`，包含 `regexp.compile`、`regexp.findString`、`regexp.matchRegexpString`、`regexp.quoteMeta`
- 已有：`strings`，包含 `strings.containsStr`、`strings.hasPrefix`、`strings.hasSuffix`、`strings.joinStr`、`strings.replaceAll`、`strings.split`、`strings.toUpper`、`strings.toLower`、`strings.trimSpace`
- 已有：`math`，包含 `math.abs`、`math.ceil`、`math.floor`、`math.round`、`math.sqrt`、`math.pow`
- 已有：`dict`，包含 `dict.fromList`、`dict.get`、`dict.insert`、`dict.remove`；当前运行时以对象承载可执行子集
- 已有：`json`，包含 `json.encode`；当前运行时以 string 承载官方 bytes 返回语义
- 已有：`runtime`，包含 `runtime.version`
- 已有：`system`，包含 `system.time`
- 已有：`types`，包含 `types.isBool`、`types.isDuration`、`types.isFloat`、`types.isInt`、`types.isNumeric`、`types.isRegexp`、`types.isString`、`types.isTime`、`types.isType`、`types.isUInt`
- 已有：`join`，包含 `join.inner`、`join.left`、`join.right`、`join.full`，支持 predicate `on` 与 `as` 输出函数；后续继续补齐更完整的官方 join 包边界
- 后续补：`timezone`，优先选择实现小、测试边界清楚、对现有 examples 有帮助的函数
- 后续评估：`influxdata/influxdb/schema`，用于承接官方 schema 探索类能力；不要先做顶层 `schema`
- 后续评估：`generate`、`sampledata`、`interpolate`，作为纯内存运行时可实现的扩展包
- 暂缓：`http`、`kafka`、`socket`、`slack`、`pagerduty`、`pushbullet` 等外部 IO / 集成包，除非后续运行时明确引入外部副作用模型
- 暂缓：`planner`、`profiler`、`testing`、`internal/*` 等引擎和测试辅助包，等解释器边界更稳定后再评估

### universe 拆分路线

官方 `universe` package 默认加载，不需要 `import`。当前顶层查询变换 builtin 继续按这个模型暴露，但不包含数据源入口。

- 已拆：`runtime/runtime_builtin_universe_core.cpp`，承载 `len`、`string`、`contains` 等基础默认 builtin
- 已拆：`runtime/runtime_builtin_universe_transform.cpp`，承载 `range`、`filter`、`map`、`keep`、`drop`、`rename`、`duplicate`、`set`、`sort`、`limit`、`tail`、`group`、`pivot`、`fill`、`union`
- 已拆：`runtime/runtime_builtin_universe_aggregate.cpp`，承载 `sum`、`mean`、`min`、`max`、`count`、`spread`、`quantile`、`median`、`first`、`last`、`top`、`bottom`、`reduce`、`distinct`
- 已拆：`runtime/runtime_builtin_universe_window.cpp`，承载 `window`、`aggregateWindow`、`elapsed`、`difference`、`derivative`
- 已拆：`runtime/runtime_builtin_universe_join.cpp`，承载当前顶层 `join()`，并复用为 `join` package 的底层实现
- 已拆：`runtime/runtime_builtin_universe_inspect.cpp`，承载 `yield`、`columns`、`keys`、`findColumn`、`findRecord` 等结果输出和检查 helper

- 已拆：`runtime/runtime_builtin_table_helpers.h`，承载通用参数校验、表/行/chunk 变换、列投影、排序比较、pivot 辅助和 builtin 安装工具
- 已拆：`runtime/runtime_builtin_time_helpers.h`，承载 RFC3339 解析/格式化、duration/window duration 解析、timezone/location 和窗口边界时间运算
- 已拆：`runtime/runtime_builtin_window_helpers.h`，承载 `window` / `aggregateWindow` 专用的窗口 row/group materialization、空窗口聚合和输出行生成
- 已拆：`runtime/runtime_builtin_aggregate_helpers.h`，承载数值聚合摘要、min/max、quantile 和按 group materialize 聚合输出行

## 建议优先补的语法

- 围绕多表流、日历窗口和更丰富聚合的更多真实查询形态
- 更多围绕多表流、日历窗口和更丰富聚合的真实查询 fixture
- 更多围绕复杂调用 / 对象组合的 malformed 输入 fixture

## 解释器路线图

长期目标仍然是一个同时具备解析与执行能力的可用 Flux 解释器。一个实际可行的顺序如下：

### 1. 解析器基线补齐

- 持续扩大已测试语法范围，直到常见查询子集稳定
- 保持 AST/debug 质量，让解析失败依旧容易诊断

### 2. 语义 / 运行时基础

- 定义运行时值类型，如 null / bool / int / uint / float / string / time / duration / regex / array / object / function
- 增加词法环境与变量、option、函数参数的作用域查找
- 定义可调用 builtin 以及标准库入口注册表

### 3. 表达式解释执行

- 求值字面量、数组、对象、字典、算术/逻辑运算、成员/索引访问和条件表达式
- 支持函数值、闭包与函数调用
- 支持运行时的 `option` 绑定与 `builtin` 声明

当前状态：

- 运行时值：已启动且可用
- 环境 / 作用域：已启动且可用
- 表达式求值：已启动且可用
- 语句执行：已启动且可用
- 函数调用与闭包：已启动且可用

### 4. 查询 / 管道执行

- 继续扩展当前内存表模型与 pipe 输入支持
- 在现有多表流基础上继续补齐剩余查询 builtin
- 继续补齐 `aggregateWindow` 的剩余 Flux 细节，例如更完整的标准库窗口语义
- 定义足够多的运行时行为，支持真实 Flux 脚本端到端运行

### 5. 诊断与可用性

- 增加带源码位置的运行时错误
- 增加脚本执行示例和解释器导向测试
- 让 `flux ast` 与运行时调试信息保持一致
- 持续提升终端 / CSV / JSON 结果格式

## 部分特性示例

这些例子很适合作为下一波解析测试，因为它们正好位于“当前可用边界”的附近。

### Label literal

示例：

```flux
.field
```

当前预期：

- AST 中已有 `LabelLit`
- 真实世界覆盖还比较薄
- 仍需要更多解析测试来确认它的合法位置以及与成员访问的交互
