# Flux C++ Playground

`cpp/pl/flux` 是一个用 C++20 写的 Flux 解析器、执行器和标准库实验场。它的目标不是立刻完整复刻 InfluxData 官方 Flux，而是提供一个可运行、可调试、可回归测试的 Flux 子集，用来逐步验证语法、运行时值模型、表流管道和内置包设计。

当前能力包括：

- Flux scanner、parser、AST 调试输出
- 表达式求值、作用域、函数、管道、顶层执行环境
- 人类可读、annotated CSV、JSON 三种 CLI 输出
- REPL、内联源码、文件执行、结果筛选
- 一批默认加载的 universe builtin
- `array`、`csv`、`date`、`dict`、`join`、`json`、`math`、`regexp`、`runtime`、`sqlite`、`strings`、`system`、`timezone`、`types` 等内置包
- `stdlib_conformance` 快照样例，约束每个已实现 builtin 的主要行为

更细的语法和运行时支持矩阵见 [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md)。

多数据源、connector、查询计划和算子下推的后续设计见
[DATASOURCE_ARCHITECTURE.md](./DATASOURCE_ARCHITECTURE.md)。

文档职责大致这样划分：

- 本 README：用户入口、构建运行方式和高层导航。
- [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md)：当前支持状态，不解释架构演进过程。
- [DATASOURCE_ARCHITECTURE.md](./DATASOURCE_ARCHITECTURE.md)：connector / optimizer / execution 架构和 roadmap。
- [benchmark/README.md](./benchmark/README.md)：benchmark 复现方法、数据形态和当前基线。
- [benchmark/OPTIMIZATION_LOG.md](./benchmark/OPTIMIZATION_LOG.md)：优化过程、取舍和性能结论解释。

## 编译依赖

项目通过 Bazel/Bzlmod 构建。Flux 子项目本身主要依赖：

- Bazel，建议使用仓库当前 Bzlmod 配置可解析的版本
- 支持 C++20 的编译器
- Abseil，用于 status、string、time 等基础库
- simdjson，用于 CLI/JSON 相关处理，也建议后续 JSON package 继续复用它，避免手写 JSON 编码/解析逻辑
- GoogleTest，用于单元测试

依赖版本在仓库根目录 [MODULE.bazel](../../../MODULE.bazel) 中声明，正常情况下不需要手动安装三方 C++ 库。

## 快速上手

构建 CLI：

```bash
bazel build //cpp/pl/flux:flux
```

执行一段 Flux：

```bash
./bazel-bin/cpp/pl/flux/flux -e 'sum([1, 2, 3])'
```

输出 JSON：

```bash
./bazel-bin/cpp/pl/flux/flux --output-format json -e 'value = 41
value + 1'
```

输出 AST：

```bash
./bazel-bin/cpp/pl/flux/flux ast -e 'data |> filter(fn: (r) => r._value > 10)'
```

执行文件：

```bash
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/scalar_basics.flux
```

只输出某个结果：

```bash
./bazel-bin/cpp/pl/flux/flux \
  --output-format json \
  --result _result \
  cpp/pl/flux/examples/stdlib_conformance/types.flux
```

启动 REPL：

```bash
./bazel-bin/cpp/pl/flux/flux
```

REPL 会复用同一个运行时环境：

```text
flux> x = 40
40
flux> x + 2
42
flux> :quit
```

常用 CLI 参数：

| 参数                                  | 说明                       |
| ------------------------------------- | -------------------------- |
| `ast`                                 | 只解析并输出 AST           |
| `-e <source>`                         | 执行内联源码               |
| `--output-format human \| csv \| json` | 切换输出格式               |
| `--result <name>`                     | 只输出指定结果             |
| `--list-results`                      | 列出脚本可输出的结果名     |
| `--quiet`                             | 执行但不打印结果           |
| `--no-prelude`                        | 不注入默认 builtin/prelude |

JSON 输出会保留逻辑表结构：每个 table chunk 包含 `table` 序号、`columns`、与列对齐的 `group` flags、可选 `groupKey` 和行数据。

## 示例与测试

快速看一组完整能力展示：

```bash
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/stdlib_packages.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
```

标准库契约样例在 [examples/stdlib_conformance](./examples/stdlib_conformance/README.md)。这里的规则是“不重不漏”：每个已实现 builtin 必须有一个主覆盖点，同一个 builtin 只在一个样例里作为主覆盖点。测试脚本会检查目录里的 `.flux` 文件和快照清单一致。

运行 conformance：

```bash
bazel test //cpp/pl/flux:stdlib_conformance_test --test_output=errors
```

运行核心单元测试：

```bash
bazel test \
  //cpp/pl/flux:scanner_unit_test \
  //cpp/pl/flux:parser_unit_test \
  //cpp/pl/flux:runtime_eval_unit_test \
  //cpp/pl/flux:runtime_exec_unit_test \
  //cpp/pl/flux:flux_cli_unit_test \
  --test_output=errors
```

Benchmark 说明见 [benchmark/README.md](./benchmark/README.md)。

## 运行时模型

运行时值模型覆盖 Flux 当前可执行子集需要的核心类型：

- `null`
- `bool`
- `int`
- `uint`
- `float`
- `string`
- `time`
- `duration`
- `regexp`
- `array`
- `object`
- `function`
- `table`

表值是内存内逻辑表流，包含 bucket、rows、tables、group key 和 result name 等信息。当前实现优先保证示例和测试中的确定行为，不追求完全模拟官方 Flux query planner。

默认 prelude 会安装 universe builtin；显式 `import` 会从本地 package registry 中加载内置包。运行时没有 universe 顶层数据源入口；所有数据源都走 provider package API，例如 `array.from`、`csv.from`、`sqlite.from`，后续外部源也按 `mysql.from` 这类形态扩展。未知 package 会被保留为 metadata object，例如 `import x "experimental/foo"` 会绑定 `{path: "experimental/foo", alias: "x"}`，方便调试导入信息。

## Universe Builtins

Universe builtin 默认注入，无需 `import`。

### 基础函数

| 函数                     | 说明                              |
| ------------------------ | --------------------------------- |
| `len(v)`                 | 返回 string、array、object 的长度 |
| `string(v)`              | 将值转成 Flux 风格字符串          |
| `contains(set:, value:)` | 判断数组中是否存在值              |

### 表变换

| 函数                                       | 说明                                           |
| ------------------------------------------ | ---------------------------------------------- |
| `range(start:, stop:)`                     | 按 `_time` 过滤时间范围                        |
| `filter(fn:, onEmpty:)`                    | 按谓词过滤行，`onEmpty: "keep"` 可保留空表形状 |
| `map(fn:)`                                 | 对每行做对象映射                               |
| `limit(n:, offset:)`                       | 取前 `n` 行                                    |
| `tail(n:, offset:)`                        | 取后 `n` 行                                    |
| `keep(columns:)`                           | 只保留指定列                                   |
| `drop(columns:)`                           | 删除指定列                                     |
| `rename(columns:)`                         | 重命名列                                       |
| `duplicate(column:, as:)`                  | 复制列                                         |
| `set(key:, value:)`                        | 给每行写入固定列值                             |
| `sort(columns:, desc:)`                    | 按列排序                                       |
| `group(columns:)`                          | 生成逻辑分组                                   |
| `pivot(rowKey:, columnKey:, valueColumn:)` | 透视行列                                       |
| `fill(column:, value:, usePrevious:)`      | 填充空值                                       |
| `union(tables:)`                           | 合并多个表流                                   |

### 聚合、选择器与排名

| 函数                     | 说明                                |
| ------------------------ | ----------------------------------- |
| `sum(arr)`               | 数组求和                            |
| `mean(arr)`              | 数组均值                            |
| `min(arr)`               | 数组最小值                          |
| `max(arr)`               | 数组最大值                          |
| `count(column:)`         | 按表或分组计数                      |
| `spread(column:)`        | 计算最大值与最小值差                |
| `quantile(q:, column:)`  | 计算分位数，`q` 支持单值或数组      |
| `median(column:)`        | 中位数，等价于 `q = 0.5` 的常用路径 |
| `first()`                | 每个表/分组第一行                   |
| `last()`                 | 每个表/分组最后一行                 |
| `top(n:, columns:)`      | 取最大 `n` 行                       |
| `bottom(n:, columns:)`   | 取最小 `n` 行                       |
| `reduce(identity:, fn:)` | 按行折叠成对象                      |
| `distinct(column:)`      | 取列去重结果                        |

### 窗口与序列函数

| 函数                                                                                          | 说明                                 |
| --------------------------------------------------------------------------------------------- | ------------------------------------ |
| `window(every:, period:, offset:, createEmpty:)`                                              | 按时间窗口重组表流                   |
| `aggregateWindow(every:, fn:, period:, offset:, location:, timeSrc:, timeDst:, createEmpty:)` | 窗口聚合，支持固定时长和部分日历窗口 |
| `elapsed(unit:)`                                                                              | 计算相邻行时间差                     |
| `difference(columns:, nonNegative:, keepFirst:)`                                              | 计算相邻行数值差                     |
| `derivative(unit:, nonNegative:, initialZero:)`                                               | 计算变化率                           |

### Join、检查与输出

| 函数                          | 说明                                             |
| ----------------------------- | ------------------------------------------------ |
| `join(tables:, on:, method:)` | 顶层 join，支持 `inner`、`left`、`right`、`full` |
| `columns()`                   | 返回表列名                                       |
| `keys()`                      | 返回 group key 列名                              |
| `findColumn(fn:, column:)`    | 找到匹配行并返回某列数组                         |
| `findRecord(fn:, idx:)`       | 找到匹配行并返回指定位置的 record                |
| `explain()`                   | 返回 logical / optimized logical / physical / pipeline plan；`pipeline: true, json: true` 可返回结构化 DAG |
| `yield(name:)`                | 设置结果名并输出表流                             |

## 内置包

### `array`

| 函数                                 | 说明               |
| ------------------------------------ | ------------------ |
| `array.from(rows:, bucket:)`         | 从对象数组构造内联内存表 |
| `array.concat(arr:, v:)`             | 拼接数组           |
| `array.filter(arr:, fn:)`            | 过滤数组元素       |
| `array.map(arr:, fn:)`               | 映射数组元素       |
| `array.contains(arr:, value:)`       | 判断数组是否包含值 |
| `array.reduce(arr:, identity:, fn:)` | 折叠数组           |
| `array.any(arr:, fn:)`               | 任一元素满足谓词   |
| `array.all(arr:, fn:)`               | 全部元素满足谓词   |
| `array.range(start:, stop:, step:)`  | 生成整数序列       |
| `array.repeat(value:, n:)`           | 重复生成值         |
| `array.length(arr:)`                 | 数组长度           |
| `array.get(arr:, index:, default:)`  | 安全索引访问       |
| `array.slice(arr:, start:, end:)`    | 切片               |
| `array.sort(arr:, desc:)`            | 标量数组排序       |
| `array.flatMap(arr:, fn:)`           | 映射后拍平         |
| `array.find(arr:, fn:, default:)`    | 查找首个匹配元素   |
| `array.findIndex(arr:, fn:)`         | 查找首个匹配索引   |
| `array.take(arr:, n:)`               | 取前 N 个元素      |
| `array.drop(arr:, n:)`               | 跳过前 N 个元素    |
| `array.reverse(arr:)`                | 反转数组           |
| `array.unique(arr:)`                 | 去重               |
| `array.unfold(seed:, fn:, limit:)`   | 按状态展开数组     |
| `array.scan(arr:, identity:, fn:)`   | 保留每步折叠结果   |
| `array.zip(left:, right:)`           | 配对两个数组       |
| `array.enumerate(arr:)`              | 添加元素索引       |

### `csv`

| 函数                           | 说明                                          |
| ------------------------------ | --------------------------------------------- |
| `csv.from(csv:, file:, mode:)` | 从 raw CSV 或 annotated CSV 字符串/文件构造表 |

`mode: "raw"` 解析普通表头 CSV；annotated CSV 支持 `#datatype`、`#group`、`#default` 和 result/table 列的常见形态。

### `sqlite`

| 函数                              | 说明                      |
| --------------------------------- | ------------------------- |
| `sqlite.from(path:, table:)`      | 从外部表扫描物化为 Flux 表流 |

`sqlite.from` 通过 SQLite C API 扫描 `path` 指向的数据库表，并将 `null`、integer、float、text、blob-as-string 映射到 Flux 运行时值。用户入口不提供 `query` 模式，SQL 只作为 SQLite connector 内部 physical plan。

支持状态见 [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md)，connector / planner / execution 设计见 [DATASOURCE_ARCHITECTURE.md](./DATASOURCE_ARCHITECTURE.md)。

当前没有顶层 `from(bucket:)` 或其他 universe 数据源占位。新增数据源时优先补 provider package，例如 `mysql.from`，而不是恢复顶层 `from`。

### `mysql`

| 函数                       | 说明                 |
| -------------------------- | -------------------- |
| `mysql.from(dsn:, table:)` | 从 MySQL 表扫描物化为 Flux 表流 |
| `mysql.from(host:, user:, password:, database:, table:, ?port:)` | 同上，使用显式连接字段 |

`mysql.from` 使用 Boost.MySQL 连接外部 MySQL，支持 `mysql://user:password@host[:port]/database` 和 `user:password@tcp(host[:port])/database` 两类 `dsn`，也支持显式 `host/user/password/database/port` 字段；`port` 默认 3306。用户入口不提供 raw `query` 模式，SQL 只作为 connector 内部 physical plan。

支持状态见 [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md)。性能对比和复现方式见 [benchmark/README.md](./benchmark/README.md)。

### `date`

| 函数                       | 说明            |
| -------------------------- | --------------- |
| `date.add(d:, to:)`        | 时间加 duration |
| `date.sub(d:, from:)`      | 时间减 duration |
| `date.truncate(t:, unit:)` | 截断到指定单位  |
| `date.year(t:)`            | 年              |
| `date.month(t:)`           | 月              |
| `date.monthDay(t:)`        | 月内日期        |
| `date.weekDay(t:)`         | 星期            |
| `date.hour(t:)`            | 小时            |
| `date.minute(t:)`          | 分钟            |
| `date.second(t:)`          | 秒              |

### `dict`

| 函数                               | 说明                           |
| ---------------------------------- | ------------------------------ |
| `dict.fromList(pairs:)`            | 从 `{key, value}` 数组构造字典 |
| `dict.get(dict:, key:, default:)`  | 读取 key，缺失时返回默认值     |
| `dict.insert(dict:, key:, value:)` | 返回插入/覆盖后的字典          |
| `dict.remove(dict:, key:)`         | 返回删除 key 后的字典          |

当前还没有独立 dict runtime type，字典由 object 承载；非 string key 会按运行时字符串化结果保存。

### `join`

| 函数                                 | 说明                             |
| ------------------------------------ | -------------------------------- |
| `join.inner(left:, right:, on:)`     | 内连接                           |
| `join.left(left:, right:, on:, as:)` | 左连接，可用 predicate/as lambda |
| `join.right(left:, right:, on:)`     | 右连接                           |
| `join.full(left:, right:, on:)`      | 全连接                           |

`join` package 是顶层 `join()` 的 facade，覆盖常用内存表连接路径。`on` 可以是 predicate 函数，也可以是列名数组；列名数组路径下可用 `leftName` / `rightName` 控制重名列输出后缀。

### `json`

| 函数              | 说明                       |
| ----------------- | -------------------------- |
| `json.encode(v:)` | 编码 Flux 值为 JSON 字符串 |

当前运行时没有 bytes 类型，所以 `json.encode` 先返回 string。后续如果增加 `json.decode` 或 bytes 值，应该直接复用 simdjson，而不是继续扩展手写 JSON 逻辑。

### `math`

| 函数               | 说明       |
| ------------------ | ---------- |
| `math.pi`          | 圆周率常量 |
| `math.abs(x:)`     | 绝对值     |
| `math.ceil(x:)`    | 向上取整   |
| `math.floor(x:)`   | 向下取整   |
| `math.round(x:)`   | 四舍五入   |
| `math.sqrt(x:)`    | 平方根     |
| `math.pow(x:, y:)` | 幂         |

### `regexp`

| 函数                               | 说明                 |
| ---------------------------------- | -------------------- |
| `regexp.compile(v:)`               | 编译正则字符串       |
| `regexp.findString(r:, v:)`        | 返回第一个匹配字符串 |
| `regexp.matchRegexpString(r:, v:)` | 判断是否匹配         |
| `regexp.quoteMeta(v:)`             | 转义正则元字符       |

### `runtime`

| 函数                | 说明                                   |
| ------------------- | -------------------------------------- |
| `runtime.version()` | 返回当前 playground runtime 版本字符串 |

### `strings`

| 函数                               | 说明                   |
| ---------------------------------- | ---------------------- |
| `strings.containsStr(v:, substr:)` | 是否包含子串           |
| `strings.hasPrefix(v:, prefix:)`   | 是否有前缀             |
| `strings.hasSuffix(v:, suffix:)`   | 是否有后缀             |
| `strings.joinStr(arr:, v:)`        | 用分隔符拼接字符串数组 |
| `strings.replaceAll(v:, t:, u:)`   | 替换全部子串           |
| `strings.split(v:, t:)`            | 分割字符串             |
| `strings.toUpper(v:)`              | 转大写                 |
| `strings.toLower(v:)`              | 转小写                 |
| `strings.trimSpace(v:)`            | 去掉首尾空白           |

### `system`

| 函数            | 说明              |
| --------------- | ----------------- |
| `system.time()` | 返回当前 UTC 时间 |

`system.time()` 是唯一一个 conformance 中用正则匹配的包函数，因为输出随执行时间变化。

### `timezone`

| 函数                         | 说明                     |
| ---------------------------- | ------------------------ |
| `timezone.utc`               | 默认 UTC location 记录   |
| `timezone.fixed(offset:)`    | 生成固定偏移 location    |
| `timezone.location(name:)`   | 生成命名时区 location    |

`timezone` 返回的 location 记录可直接传给 `window(location:)`、`aggregateWindow(location:)`，也可用于 `option location = ...`。

### `types`

| 函数                      | 说明                      |
| ------------------------- | ------------------------- |
| `types.isNumeric(v:)`     | 是否为 int、uint 或 float |
| `types.isType(v:, type:)` | 按类型名判断              |
| `types.isString(v:)`      | 是否为 string             |
| `types.isDuration(v:)`    | 是否为 duration           |
| `types.isBool(v:)`        | 是否为 bool               |
| `types.isInt(v:)`         | 是否为 int                |
| `types.isUInt(v:)`        | 是否为 uint               |
| `types.isFloat(v:)`       | 是否为 float              |
| `types.isTime(v:)`        | 是否为 time               |
| `types.isRegexp(v:)`      | 是否为 regexp             |

这里没有严格照搬官方 `types` 包的最小 API，而是按当前 runtime 值模型扩展了一批直接可用的 `isXxx` helper，方便样例和后续包实现做类型分派。

## Language Server (LSP)

`contrib/lsp/` 提供了一个基于 stdio JSON-RPC 2.0 的 Flux 语言服务器 `flux-ls`，可以集成到 neovim、VS Code 等编辑器中。

构建：

```bash
bazel build //cpp/pl/flux/contrib/lsp:flux-ls
```

### 支持的 LSP 能力

| 能力 | 说明 |
| ---- | ---- |
| `textDocument/publishDiagnostics` | 实时语法诊断，基于 parser 错误推送 |
| `textDocument/completion` | 关键字、内置包和函数补全 |
| `textDocument/hover` | 标识符悬浮提示 |
| `textDocument/formatting` | 全文档格式化 |

### Formatter 规则

格式化器遍历 AST 输出规范化 Flux 源码，主要规则如下：

- **Pipe chain 展开**：`a |> b() |> c()` 扁平化为同一缩进层级，每个 `|>` 独占一行。
- **行宽感知**：调用表达式 inline 后超过 `max_line_width`（默认 120）时自动展开为多行，每个参数独占一行。
- **复杂度驱动**：参数包含 pipe 表达式、block 函数体或深层嵌套对象时强制展开，不依赖宽度。
- **ObjectExpr 透明解包**：Flux 将 `foo(a: 1, b: 2)` 解析为 `CallExpr(foo, [ObjectExpr({a:1, b:2})])`，格式化时透明地将 ObjectExpr 属性展开为直接的命名参数。
- **Trailing comma**：多行展开模式下所有参数（包括最后一个）都保留尾逗号，方便 diff。
- **语句间距**：连续的简单变量赋值紧凑排列不加空行；复杂赋值（多行 call、pipe chain）与相邻语句之间自动插入空行。

### 配置

flux-ls 支持三层配置，优先级从低到高：

1. **命令行参数**（启动时指定）
2. **LSP initializationOptions**（客户端在 `initialize` 请求中传入）
3. **每次格式化请求的 `options`**（仅 `tabSize` / `insertSpaces`）

#### 命令行参数

```
flux-ls [OPTIONS]

Options:
  --max-line-width=N   格式化行宽阈值 (默认: 120)
  --indent-width=N     缩进空格数 (默认: 4)
  --use-tabs           使用 Tab 缩进
  --help, -h           显示帮助
  --version, -v        显示版本
```

#### LSP initializationOptions

客户端可在 `initialize` 请求的 `initializationOptions` 中传入以下字段：

| 字段 | 类型 | 默认值 | 说明 |
| ---- | ---- | ------ | ---- |
| `maxLineWidth` | int | 120 | 行宽阈值，超出时展开为多行 |
| `indentWidth` | int | 4 | 缩进空格数 |
| `useTabs` | bool | false | 是否使用 Tab 缩进 |

`initializationOptions` 会覆盖命令行参数。编辑器每次格式化请求中的 `tabSize` / `insertSpaces` 会进一步覆盖 `indentWidth` / `useTabs`（但不影响 `maxLineWidth`）。

### Neovim 配置示例

在 `~/.config/nvim/lua/plugins/lsp/servers/` 下新建 `flux_ls.lua`：

```lua
return {
  cmd = {
    vim.fn.expand("~/workspace/liubang/playground/bazel-bin/cpp/pl/flux/contrib/lsp/flux-ls"),
    "--max-line-width=120",
  },
  filetypes = { "flux" },
  root_markers = { ".git" },
  init_options = {
    maxLineWidth = 120,
    indentWidth = 4,
    useTabs = false,
  },
}
```

并在 LSP 初始化列表中加入 `flux_ls`。命令行参数和 `init_options` 二选一即可，`init_options` 优先级更高。

## 代码结构

| 文件/模块                          | 职责                       |
| ---------------------------------- | -------------------------- |
| `common/`                          | 共享兼容层                 |
| `syntax/scanner.*`、`syntax/token.h` | 词法扫描                  |
| `syntax/parser.*`、`syntax/ast.*`、`syntax/ast_debug.*` | 语法解析、AST 与调试输出 |
| `runtime/runtime_value.*`          | 运行时值模型               |
| `runtime/runtime_env.*`            | 环境与作用域               |
| `runtime/runtime_eval.*`           | 表达式求值                 |
| `runtime/runtime_exec.*`           | 文件级语句执行             |
| `runtime/runtime_builtin_universe_*.cpp` | 默认 universe builtin |
| `runtime/runtime_builtin_table.cpp` | `array`、`csv` 包         |
| `runtime/runtime_builtin_sqlite.cpp` | `sqlite` 包和 SQLite table scan 入口 |
| `runtime/runtime_builtin_mysql.cpp` | `mysql` 包骨架            |
| `runtime/runtime_builtin_scalar.cpp` | 标量类 stdlib 包          |
| `runtime/runtime_builtin_package.*` | 包注册与导入              |
| `connector/`、`optimizer/`、`execution/`、`plan/` | datasource、优化、物理执行与计划 IR |
| `contrib/lsp/`                     | Language Server（诊断、补全、悬浮、格式化） |
| `cli/flux_cli.*`、`cli/flux.cpp`   | CLI、REPL、输出格式        |
| `*/tests/`                         | 各模块单测和脚本测试       |

内部 helper 按职责拆分：

- `runtime/runtime_builtin_table_helpers.h`
- `runtime/runtime_builtin_time_helpers.h`
- `runtime/runtime_builtin_window_helpers.h`
- `runtime/runtime_builtin_aggregate_helpers.h`

## 已知限制

- 这是内存内执行器，没有连接真实 InfluxDB storage。
- 类型系统仍是运行时动态检查，尚未实现 Flux 完整静态类型推导。
- table/group/window 行为覆盖常用路径，但不保证与官方 planner 完全一致。
- `dict` 暂由 object 承载。
- `json.encode` 暂返回 string，尚无 bytes 类型。
- `csv.from` 覆盖 raw/annotated 常见形态，不是完整 CSV 方言实现。
- SQL provider 当前只暴露 `table` 模式；`sqlite.from` 和 `mysql.from` 都不提供 raw `query` 入口。
- 复杂 `filter(fn:)`、跨源 join 下推和更多 SQL 方言仍未实现。
- package 覆盖以当前 `stdlib_conformance` 为准，新增函数需要同步样例和快照。

## 贡献时的收尾清单

新增或修改 builtin 时，请至少同步：

1. 实现代码与必要单元测试。
2. [examples/stdlib_conformance](./examples/stdlib_conformance/README.md) 的样例和覆盖表。
3. `//cpp/pl/flux:stdlib_conformance_test` 快照。
4. 本 README 的函数清单。
5. [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md) 中对应能力状态。
