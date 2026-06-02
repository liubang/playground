# Flux C++ Playground

`cpp/pl/flux` 是一个用 C++20 写的 Flux 解析器、执行器和标准库实验场。它的目标不是立刻完整复刻 InfluxData 官方 Flux，而是提供一个可运行、可调试、可回归测试的 Flux 子集，用来逐步验证语法、运行时值模型、表流管道和内置包设计。

当前能力包括：

- Flux scanner、parser、AST 调试输出
- 表达式求值、作用域、函数、管道、顶层执行环境
- 共享 builtin metadata、semantic model 和完整 analyzer，用于包/函数签名、作用域、定义/引用解析、表达式类型、record/table schema、绑定/类型诊断和 LSP 能力
- 人类可读、annotated CSV、JSON 三种 CLI 输出
- REPL、内联源码、文件执行、结果筛选
- Universe builtin + `array`、`csv`、`date`、`dict`、`join`、`json`、`math`、`regexp`、`runtime`、`sqlite`、`mysql`、`strings`、`system`、`timezone`、`types` 等内置包
- Connector 框架（SQLite / MySQL）、RBO + CBO 优化器、Pipeline 执行引擎
- Language Server (LSP)：诊断、补全、悬浮、定义跳转、引用、重命名、semantic tokens、格式化

## 文档

| 文档 | 说明 |
| ---- | ---- |
| [docs/architecture.md](docs/architecture.md) | 系统架构概览：编译/执行管线、组件关系、设计决策 |
| [docs/stdlib-reference.md](docs/stdlib-reference.md) | Universe builtin 和内置包完整 API 参考 |
| [docs/datasource-evolution.md](docs/datasource-evolution.md) | 数据源 / 优化器 / 执行引擎演进日志 |
| [docs/support-matrix.md](docs/support-matrix.md) | 语法和运行时功能实现状态矩阵 |
| [benchmark/README.md](benchmark/README.md) | Benchmark 复现方法、数据形态和当前基线 |
| [examples/README.md](examples/README.md) | 示例目录索引 |

## 编译依赖

项目通过 Bazel/Bzlmod 构建。主要依赖：

- Bazel，建议使用仓库当前 Bzlmod 配置可解析的版本
- 支持 C++20 的编译器
- Abseil，用于 status、string、time 等基础库
- simdjson，用于 CLI/JSON 相关处理
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

启动 REPL：

```bash
./bazel-bin/cpp/pl/flux/flux
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

## 示例与测试

快速看一组完整能力展示：

```bash
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/feature_gallery/stdlib_packages.flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
```

标准库契约样例在 [examples/stdlib_conformance](./examples/stdlib_conformance/README.md)。运行 conformance：

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

运行时值模型覆盖 Flux 当前可执行子集需要的核心类型：`null`、`bool`、`int`、`uint`、`float`、`string`、`time`、`duration`、`regexp`、`array`、`object`、`function`、`table`。

表值是内存内逻辑表流，包含 bucket、rows、tables、group key 和 result name 等信息。默认 prelude 会安装 universe builtin；显式 `import` 会从本地 package registry 中加载内置包。所有数据源都走 provider package API（`array.from`、`csv.from`、`sqlite.from`、`mysql.from`）。

完整 API 清单见 [docs/stdlib-reference.md](docs/stdlib-reference.md)。

## Language Server (LSP)

`contrib/lsp/` 提供了一个基于 stdio JSON-RPC 2.0 的 Flux 语言服务器 `flux-ls`，可以集成到 neovim、VS Code 等编辑器中。

```bash
bazel build //cpp/pl/flux/contrib/lsp:flux-ls
```

支持的能力：实时语法/语义诊断、关键字和函数补全、悬浮签名、定义跳转、引用查找、重命名、semantic tokens、全文档格式化。

格式化规则：pipe chain 展开、行宽感知自动多行、复杂度驱动展开、ObjectExpr 透明解包、trailing comma。支持命令行参数、`initializationOptions`、格式化请求三层配置。

## 代码结构

| 模块                          | 职责                       |
| ----------------------------- | -------------------------- |
| `syntax/`                     | 词法扫描、语法解析、AST    |
| `analysis/`                   | 语义分析、builtin metadata |
| `runtime/`                    | 值模型、求值、执行、builtin 实现 |
| `connector/`                  | 数据源连接器（SQLite、MySQL） |
| `optimizer/`                  | RBO + CBO 查询优化         |
| `plan/`                       | 逻辑/物理计划 IR           |
| `execution/`                  | 物理计划、Pipeline 调度、执行 profile |
| `cli/`                        | CLI、REPL、输出格式        |
| `contrib/lsp/`                | Language Server             |
| `benchmark/`                  | 性能基准测试               |
| `examples/`                   | 使用示例和 conformance     |

## 已知限制

- 内存内执行器，没有连接真实 InfluxDB storage
- 类型系统是运行时动态检查，尚未实现完整静态类型推导
- table/group/window 行为覆盖常用路径，不保证与官方 planner 完全一致
- SQL provider 当前只暴露 `table` 模式，不提供 raw `query` 入口
- package 覆盖以 `stdlib_conformance` 为准

## 贡献时的收尾清单

新增或修改 builtin 时，请至少同步：

1. 实现代码与必要单元测试
2. [examples/stdlib_conformance](./examples/stdlib_conformance/README.md) 的样例和覆盖表
3. `//cpp/pl/flux:stdlib_conformance_test` 快照
4. [docs/stdlib-reference.md](docs/stdlib-reference.md) 的函数清单
5. [docs/support-matrix.md](docs/support-matrix.md) 中对应能力状态
