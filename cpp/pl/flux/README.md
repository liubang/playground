# Flux 解析器与运行时基础

`cpp/pl/flux` 目前提供了一个可执行、可调试、可逐步扩展的 Flux 子集实现，主要包括：

- AST 调试入口，可将 Flux 程序输出为树形文本或 JSON
- 一套覆盖常见语法形态的解析器测试
- 运行时基础设施：值模型、环境与作用域、表达式求值、语句执行
- 一组面向查询场景的内存内 builtins，可运行常见的 Flux 管道

如果你想看逐项能力清单，请直接查看 [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md)。
后续迭代默认遵循 [CLAUDE.md](./CLAUDE.md) 中定义的流程、测试、benchmark、examples 校验和文档同步规则。

## 构建

```bash
bazel build //cpp/pl/flux:flux
```

可选：

```bash
bazel build //cpp/pl/flux:parser_test
```

本地可重复基准测试说明见 [benchmark/README.md](./benchmark/README.md)。

## 使用方式

从标准输入读取并输出 AST：

```bash
./bazel-bin/cpp/pl/flux/flux ast <<'EOF'
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
EOF
```

从文件读取：

```bash
./bazel-bin/cpp/pl/flux/flux ast path/to/query.flux
```

直接解析内联源码：

```bash
./bazel-bin/cpp/pl/flux/flux ast -e 'value = 1 + 2'
```

执行 Flux 源码：

```bash
./bazel-bin/cpp/pl/flux/flux -e 'sum([1, 2, 3])'
```

切换结构化输出格式：

```bash
./bazel-bin/cpp/pl/flux/flux --output-format csv -e 'value = 41
value + 1'
```

对查询型脚本，CLI 会输出带结果名的结果块和逻辑表：

```text
Result: data
Table: bucket=csv, rows=1, tables=1
+==================================================+
| _time                  | _measurement | _value |
+==================================================+
| "2024-01-01T00:00:00Z" | "cpu"        | "95.5" |
+==================================================+
```

同一结果也可以导出为注解 CSV：

```text
#datatype,string,long,string,string,string
#group,false,false,false,false,false
#default,data,,,,
,result,table,_time,_measurement,_value
,data,0,2024-01-01T00:00:00Z,cpu,95.5
```

或者输出为 JSON：

```bash
./bazel-bin/cpp/pl/flux/flux --output-format json -e 'value = 41
value + 1'
```

```json
{"package":null,"imports":[],"results":[{"name":"value","value":41},{"name":"_result","value":42}],"last":42}
```

执行一个查询文件：

```bash
cat > /tmp/query.flux <<'EOF'
import "csv"

data = csv.from(
    csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
    mode: "raw",
)
    |> filter(fn: (r) => r._measurement == "cpu")
    |> limit(n: 1)
EOF

./bazel-bin/cpp/pl/flux/flux /tmp/query.flux
```

执行仓库内置的端到端示例：

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
```

这个场景见 [examples/ops_dashboard/README.md](./examples/ops_dashboard/README.md)。它覆盖了 `aggregateWindow + join`、`group + sort + elapsed`、`group + sort + difference`、`group + sort + derivative`、`aggregateWindow + union + pivot`、日历窗口、`distinct`、`union`、`reduce`、`last`，以及可通过 `--result` 缩窄的多结果脚本。

如果你想快速扫一遍当前支持的 builtin 组合，建议从 [examples/feature_gallery/README.md](./examples/feature_gallery/README.md) 开始。其中新增的 `task_driven_rollup.flux` 还专门覆盖了 `option task = {...}` 驱动的窗口查询、block-body helper 和对象返回。

当前还支持一些表检查辅助函数：

- `columns()`
- `keys()`
- `findColumn(fn:, column:)`
- `findRecord(fn:, idx:)`

`aggregateWindow()` 目前已经覆盖比较实用的一批参数组合，包括：

- 固定时长窗口与日历窗口
- `location`
- `timeSrc`
- `timeDst`
- `period`
- 固定时长窗口的 `offset`
- 日历窗口的 `offset`
- `every != period` 的重叠窗口
- `createEmpty`

## REPL

启动 REPL：

```bash
./bazel-bin/cpp/pl/flux/flux
```

在 REPL 中，`help`、`:help`、`.help` 都可以查看内置命令。

REPL 共享同一个运行时环境，因此后续输入可以直接使用前面的绑定：

```text
flux> x = 40
40
flux> x + 2
42
flux> :quit
```

多行输入会缓冲到表达式或语句看起来完整为止：

```text
flux> config = {
....> host: "local",
....> port: 8080,
....> }
{host: "local", port: 8080}
flux> config.host
"local"
```

默认情况下，运行时会安装当前 builtin prelude。标量表达式仍然会直接输出紧凑值；查询型脚本则会输出带结果名的结果块。常用参数如下：

- `--list-results`：只列出当前脚本可用的结果名
- `--output-format human|csv|json`：切换输出格式
- `--annotated-csv`：兼容别名，等同于 CSV 输出
- `--result <name>`：多结果脚本中只输出某一个结果
- `--quiet`：抑制值输出
- `--no-prelude`：仅执行显式声明或导入的符号

当脚本通过 `yield(name: "...")` 或顶层赋值产生多个结果时，`--result <name>` 会在 human、CSV、JSON 三种模式下都只保留对应结果：

```bash
./bazel-bin/cpp/pl/flux/flux --list-results cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --result latest_east_mem cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --output-format json --result latest_west_cpu cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
```

## 编辑器支持

目录中自带一个项目级 Vim 语法文件：

[`vim/syntax/flux.vim`](./vim/syntax/flux.vim)

该语法规则基于 `cpp/pl/flux` 当前已实现的 Flux 子集，包括：

- `package`、`import`、`option`、`builtin`、`testcase`、`return`
- `if` / `then` / `else`、`exists`、`and` / `or` / `not`
- 属性与注解，例如 `@trace(...)`
- 字符串、字符串插值、正则、时间、duration、无符号整数
- 类型关键字，例如 `where`、`dynamic`、`vector`、`stream`
- 当前查询/builtin 面，包括 `from`、`range`、`filter`、`map`、`aggregateWindow`、`join`、`pivot`、`yield`、`csv.from`

安装到 Vim：

```bash
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp cpp/pl/flux/vim/syntax/flux.vim ~/.vim/syntax/flux.vim
printf 'au BufRead,BufNewFile *.flux setfiletype flux\n' > ~/.vim/ftdetect/flux.vim
```

安装到 Neovim：

```bash
mkdir -p ~/.config/nvim/syntax ~/.config/nvim/ftdetect
cp cpp/pl/flux/vim/syntax/flux.vim ~/.config/nvim/syntax/flux.vim
printf 'au BufRead,BufNewFile *.flux setfiletype flux\n' > ~/.config/nvim/ftdetect/flux.vim
```

如果你不想复制文件，也可以把仓库中的 `cpp/pl/flux/vim` 加入 `runtimepath`，再在编辑器配置里把 `*.flux` 注册为 `flux` 文件类型。

## AST 输出示例

输入：

```flux
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
```

树形输出：

```text
File name="<stdin>"
PackageClause name=metrics
ImportDeclaration path="array"
VariableAssignment id=config
|  `- ObjectExpr with=base
|     |- Property key=enabled
|     |  `- BooleanLit value=true
|     `- Property key=tags
|        `- ArrayExpr
|           |- StringLit value="cpu"
|           `- StringLit value="mem"
VariableAssignment id=status
   `- ConditionalExpr
      |- UnaryExpr op=exists
      |  `- MemberExpr property=enabled
      |     `- Identifier name=config
      |- StringLit value="ok"
      `- StringLit value="missing"
```

JSON 输出：

```json
{"type":"File","summary":"name=<stdin>","children":[{"type":"PackageClause","summary":"name=metrics","children":[]},{"type":"ImportDeclaration","summary":"path=\"array\"","children":[]},{"type":"VariableAssignment","summary":"id=config","children":[{"type":"ObjectExpr","summary":"with=base","children":[{"type":"Property","summary":"key=enabled","children":[{"type":"BooleanLit","summary":"value=true","children":[]}]},{"type":"Property","summary":"key=tags","children":[{"type":"ArrayExpr","summary":"","children":[{"type":"StringLit","summary":"value=\"cpu\"","children":[]},{"type":"StringLit","summary":"value=\"mem\"","children":[]}]}]}]}]},{"type":"VariableAssignment","summary":"id=status","children":[{"type":"ConditionalExpr","summary":"","children":[{"type":"UnaryExpr","summary":"op=exists","children":[{"type":"MemberExpr","summary":"property=enabled","children":[{"type":"Identifier","summary":"name=config","children":[]}]}]},{"type":"StringLit","summary":"value=\"ok\"","children":[]},{"type":"StringLit","summary":"value=\"missing\"","children":[]}]}]}]}
```

## 测试

直接运行回归测试：

```bash
bazel test //cpp/pl/flux:parser_unit_test
bazel test //cpp/pl/flux:scanner_unit_test
```

查看完整解析器测试输出：

```bash
bazel test //cpp/pl/flux:parser_unit_test --test_output=all
```

当前单元测试覆盖的关键路径包括：

- `package` / `import`
- `builtin` + function type + `where`
- `testcase ... extends ...`
- `option task = {...}` 程序形态
- 字符串插值
- 正则匹配
- record update `{base with ...}`
- 数组 / 字典 / 布尔 / 浮点 / duration
- `if exists ... then ... else ...`
- 简写箭头函数和块体箭头函数
- `filter` / `map` / `aggregateWindow` / `join` / `pivot` / `union` / `reduce` 等真实查询形态
- 运行时值创建、深比较与对象读取
- 运行时环境的作用域链与 option 查询
- 表达式求值：字面量、标识符、数组/对象、成员/索引访问、一元/二元/逻辑运算、条件表达式、字符串插值、record update、函数值、函数调用
- 语句执行：变量赋值、`option` 赋值、表达式语句、block/return
- `testcase` 在隔离子作用域中的执行，以及 `__flux.testcase.<name>` 结果暴露
- 文件级顺序执行
- 顶层 `builtin` 声明
- 包与导入元数据处理
- 通过 `flux` 进行命令行执行、文件执行和 REPL 交互
- CLI 单测会执行所有已检入的 `.flux` examples，确保 sample 和当前实现不会悄悄漂移
- `from |> range |> filter |> map` 以及 `limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`、`reduce`、`sort`、`group`、`pivot`、`fill`、`elapsed`、`difference`、`derivative`、`count`、`first`、`last`、`union`、轻量 `join`、首版 `aggregateWindow`、Flux 风格 `csv.from`，以及 `array.concat` / `array.filter` / `array.map` / `array.contains` / `array.reduce` / `array.any` / `array.all`
- 树形 dump / JSON dump

## 运行时状态

运行时仍处于持续扩展阶段，但已经有一批稳定的基础积木：

- `runtime_value`：运行时值类型，支持 null、bool、int、uint、float、string、duration、time、regex、array、object，以及内存内表值
- `runtime_env`：词法环境，支持父作用域、变量绑定、option 绑定和最近作用域赋值
- `runtime_builtin`：内置函数注册表，目前包含 `len`、`string`、`contains`、`sum`、`mean`、`min`、`max`，以及查询相关 builtin：`from`、`range`、`filter`、`map`、`limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`、`reduce`、`sort`、`group`、`pivot`、`fill`、`elapsed`、`difference`、`derivative`、`distinct`、`count`、`first`、`last`、`union`、`join`、`aggregateWindow`、`yield`，另外通过 `import "array"` / `import "csv"` 暴露 `array.from` / `array.concat` / `array.filter` / `array.map` / `array.contains` / `array.reduce` / `array.any` / `array.all` / `csv.from`
- `runtime_eval`：表达式求值器，支持函数值与函数调用
- `runtime_exec`：语句执行器，支持赋值、`option`、表达式语句、block/return 控制流
- `flux_cli`：围绕解析器与运行时的 CLI/REPL 包装层

### 多表流与 `group` 语义

当前实现已经不再是“单张表 + 每行 `_group` 标签”的旧模型。`TableValue` 现在可以承载一个逻辑上的多表流，既保留扁平 `rows` 视图，也维护真正的逻辑表分片。

`group()` 的行为现在更接近官方 Flux：

- 会根据 group key 重新分表，而不是只打标签
- 支持 `mode: "by"` 与 `mode: "except"`
- `columns: []` 可以把数据 ungroup 成单个逻辑表
- 下游 `sort`、`distinct`、`count`、`first`、`last`，以及 `filter` / `map` / `limit` / `tail` / `keep` / `drop` / `rename` / `duplicate` / `set` / `reduce` 等算子都会按逻辑表逐表执行

运行时仍然会在每行上保留 `_group` 对象，目的是让 CSV 输出、调试和某些辅助逻辑更容易观察 group key；但真实语义已经由多表结构驱动，而不是依赖 `_group` 这个附加字段本身。

### `join`

当前 `join()` 仍然是 universe 包里的简化版双流 inner join，但它已经补上了和多表模型强相关的关键语义：

- 只支持两个输入流
- 支持 `method: "inner"`，其他 join method 还未实现
- 只会比较 group key 实例相同的逻辑表
- 输出仍然是多逻辑表流，而不是把所有匹配行压平成单表
- 输出 schema 与 group key 取两侧并集
- 两侧都存在、且不在 `on` 里的重复列会按官方旧版 `join()` 语义重命名为 `<column>_<table>`
- `null` 或缺失的 join key 不会被视为相等

这也意味着，当你要 join 两个不同 measurement 或 field 的结果时，通常要先用 `group()` 去掉会阻止匹配的 group key 列，再做 join。例如 CPU 和内存聚合结果经常需要先 regroup 掉 `_measurement`，否则两边逻辑表不会被拿来比较。

### 结果输出

文件执行会维护一个有序的命名结果列表，来源包括顶层赋值、表达式、`option`、`testcase`，以及查询管道中的 `yield(name: "...")`。

CLI 基于这份结果列表提供三种输出模式：

- `human`：人类可读的结果块与逻辑表
- `csv`：Flux 风格注解 CSV
- `json`：结构化结果对象

与之前相比，当前输出层已经能够更好地保留多表信息：

- human 输出会显示 `tables=<n>`，多逻辑表时逐表展开
- CSV 会为每个逻辑表输出一段独立的注解块，并尽量复用现有 `result` / `table` 列
- JSON 会暴露结果名、表元数据，以及逻辑表列表，方便脚本消费

这仍然不是完整的官方 Influx 结果流实现，但已经从“轻量兼容层”推进到“真实多表 + 多格式输出”的阶段。

### `aggregateWindow`

当前 `aggregateWindow` 仍是内存内实现，但已经覆盖一批非常实用的语义：

- 基于 RFC3339 `_time` 的固定时长窗口，例如 `1m`
- 带时区感知边界的 `mo` / `y` 日历窗口
- `location`
- `column`
- 固定时长窗口的 `offset`
- `timeSrc`
- `timeDst`
- `period`
- 负 `period`
- `every != period` 时的重叠窗口
- `createEmpty: true`
- 聚合函数 `mean` / `sum` / `min` / `max`
- 自定义数组函数
- 窗口版 `count`
- selector 风格 `first` / `last` 的空窗口丢弃行为

窗口输出现在会同时保留逻辑表边界和 group key，不会再把不同逻辑表里同名的 group 悄悄合并；这样后续再接 `first` / `last` / `count` / `sort` 等操作时，行为会更接近官方 Flux。

尚未完成的点主要包括：

- 更完整的标准库窗口语义细节

### 表达式求值支持

当前表达式求值器支持：

- 字面量
- 标识符
- 数组与对象
- `{base with ...}` 形式的基础 record update
- 成员与索引访问
- 一元 `not`、一元 `-`、`exists`，其中一元 `-` 也支持 duration 字面量，例如 `range(start: -1h)`
- 算术 / 比较 / 逻辑表达式
- 条件表达式
- 字符串插值
- 正则匹配
- 带词法闭包捕获的用户自定义函数
- builtin 与用户函数调用
- 通过 `|>` 把值转发到 builtin 和用户定义 `<-pipe` 参数
- 基于数组的小型数值聚合
- 以内存表为基础的查询管道执行

### CSV 输入

通过 `import "csv"` 可以使用 Flux 风格的 `csv.from`。

它支持：

- `csv:` 直接传入内联 CSV 文本
- `file:` 从本地文件读取
- `mode: "annotations"`，默认模式
- `mode: "raw"`，将首行视为表头并把单元格读为字符串

示例：

```flux
import "csv"
builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]

data = csv.from(
    csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
    mode: "raw",
)
    |> filter(fn: (r) => r._measurement == "cpu")
```

从文件读取：

```flux
import "csv"

data = csv.from(file: "path/to/data.csv", mode: "raw")
```

注解 CSV 支持常见的元数据行：

- `#datatype`
- `#group`
- `#default`

并能把以下类型解析为运行时值：

- `string`
- `long`
- `unsignedLong`
- `double`
- `boolean`
- `dateTime:RFC3339`
- `dateTime:RFC3339Nano`
- `duration`

重复出现的 metadata/header block 也可以被同一个输入载荷接收，因此单个 CSV 文件可以表示多个逻辑表。

```flux
import "csv"

data = csv.from(csv: "#datatype,string,long,dateTime:RFC3339,string,double,boolean\n#group,false,false,true,true,false,false\n#default,_result,,,,,\n,result,table,_time,_measurement,_value,active\n,,0,2024-01-01T00:00:00Z,cpu,95.5,true\n")
```

这里依然是“内存内 Flux 子集实现”，不是完整的官方流式执行引擎，所以更广泛的 CSV 标准库接口还没有全部补齐。

### `array` package

通过 `import "array"`，现在除了 `array.from` 之外，也支持几类常用数组 helper：

- `array.concat(arr:, v:)`，或 `[1, 2] |> array.concat(v: [3])`
- `array.filter(arr:, fn:)`，返回满足谓词的新数组
- `array.map(arr:, fn:)`，逐元素映射出新数组
- `array.contains(arr:, value:)`，判断数组中是否存在某个值
- `array.reduce(arr:, identity:, fn:)`，按元素折叠出一个标量或对象值；当前 `fn` 以 `(element, accumulator)` 顺序接参
- `array.any(arr:, fn:)`，判断是否存在任一元素满足谓词
- `array.all(arr:, fn:)`，判断是否所有元素都满足谓词

示例：

```flux
import "array"

rows = [1, 2, 3]
    |> array.concat(v: [4])
    |> array.filter(fn: (x) => x >= 2)
    |> array.map(fn: (x) => ({host: "edge-${x}", _value: x * 10}))

hosts = rows |> array.map(fn: (r) => r.host)
hasEdge4 = hosts |> array.contains(value: "edge-4")
summary = hosts
    |> array.reduce(
        identity: {count: 0, last: ""},
        fn: (host, accumulator) => ({
            count: accumulator.count + 1,
            last: host,
        }),
    )
hasHotRow = rows |> array.any(fn: (r) => r._value >= 40)
allNamed = rows |> array.all(fn: (r) => exists r.host)

data = array.from(rows: rows)
```

## 基准测试

项目内置了本地基准测试流程，说明见 [benchmark/README.md](./benchmark/README.md)。

典型流程如下：

```bash
bazel build //cpp/pl/flux:flux
python3 cpp/pl/flux/benchmark/generate_benchmark_data.py
python3 cpp/pl/flux/benchmark/run_benchmarks.py
```

截至 2026-04-22 的一组本地基线在 [benchmark/README.md](./benchmark/README.md) 中维护。当前实现形态大体可以概括为：

- `csv.from` 先把整份输入读入内存
- 表以逻辑多表流 + 行向量形式保存在内存里
- `filter`、`map`、`sort`、`group`、`pivot`、`union` 之类算子仍会进行一定程度的数据复制或重排
- `join` 已从早期的轻量路径演进到更适合当前规模的哈希索引实现

和官方 Flux 对齐的几条关键表语义也已经固定下来：

- `filter()` 默认按 `onEmpty: "drop"` 处理，过滤后变空的逻辑表会直接从 table stream 里移除
- 显式写 `onEmpty: "keep"` 时，空逻辑表会被保留，并继续带着 group key / 列元数据流向后续算子
- `count()` 对保留下来的空逻辑表会输出一行 `_value = 0` 的结果

因此它现在更适合：

- 本地 CSV 数据探索
- 中小规模回归测试
- 查询语义实验与功能补齐

## 说明

- `flux ast` 会先输出解析错误，再输出当前 AST，并返回非零退出码。
- AST 调试输出已经覆盖大量顶层与嵌套节点的位置，包括语句、表达式、block、property、数组项、字典项、类型约束、函数参数与类型参数。
- 源码位置信息还没有覆盖全部 AST 节点，因此少数调试摘要仍然没有位置数据。
