<h1 align="center">Playground</h1>

<p align="center">
  <a href="https://liubang.github.io/playground/cpp/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/cpp/badge.json&style=flat-square&logo=cplusplus&logoColor=white" alt="C++" />
  </a>
  &nbsp;
  <a href="https://liubang.github.io/playground/java/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/java/badge.json&style=flat-square&logo=data:image/svg%2Bxml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0id2hpdGUiIGQ9Ik04Ljg1MSAxOC41NnMtLjkxNy41MzQuNjUzLjcxNGMxLjkwMi4yMTggMi44NzQuMTg3IDQuOTY5LS4yMTEgMCAwIC41NTIuMzQ2IDEuMzIxLjY0Ni00LjY5OSAyLjAxMy0xMC42MzMtLjExOC02Ljk0My0xLjE0OW0tLjU3NS0yLjYyN3MtMS4wMjguNzYyLjU0My45MjRjMi4wMzIuMjEgMy42MzYuMjI3IDYuNDEzLS4zMDggMCAwIC4zODQuMzg5Ljk4Ny42MDItNS42NzkgMS42NjEtMTIuMDA3LjEzLTcuOTQzLTEuMjE4bTQuODQtNC40NThjMS4xNTggMS4zMzMtLjMwNCAyLjUzMy0uMzA0IDIuNTMzczIuOTM5LTEuNTIgMS41ODktMy40MThjLTEuMjYxLTEuNzcyLTIuMjI4LTIuNjUyIDMuMDA3LTUuNjg4LS4wMDEgMC04LjIxNiAyLjA1MS00LjI5MiA2LjU3M00xOS4xMTYgMjAuOTU4cy42NzkuNTU5LS43NDcuOTkyYy0yLjcxMi44MjItMTEuMjg4IDEuMDY5LTEzLjY2OS4wMzMtLjg1Ni0uMzczLjc1LS44OSAxLjI1NC0uOTk5LjUyNy0uMTE0LjgyOC0uMDkzLjgyOC0uMDkzLS45NTMtLjY3MS02LjE1NiAxLjMxNy0yLjY0MyAxLjg4NyA5LjU4IDEuNTUzIDE3LjQ2Mi0uNyAxNC45NzctMS44Mk05LjI5MiAxMy4yMXMtNC4zNjIgMS4wMzYtMS41NDQgMS40MTJjMS4xODkuMTU5IDMuNTYxLjEyMyA1Ljc3LS4wNjMgMS44MDYtLjE1MiAzLjYxOC0uNDc3IDMuNjE4LS40NzdzLS42MzcuMjcyLTEuMDk4LjU4N2MtNC40MjkgMS4xNjUtMTIuOTg2LjYyMy0xMC41MjItLjU2OSAyLjA4Mi0xLjAwNSAzLjc3Ni0uODkgMy43NzYtLjg5bTcuODI0IDQuMzc0YzQuNTAzLTIuMzQgMi40MjEtNC41ODkuOTY4LTQuMjg1LS4zNTUuMDc0LS41MTUuMTM4LS41MTUuMTM4cy4xMzItLjIwNy4zODUtLjI5N2MyLjg3NS0xLjAxMSA1LjA4NiAyLjk4MS0uOTI4IDQuNTYyIDAgMCAuMDctLjA2Mi4wOS0uMTE4TTE0LjQwMS4yOXMyLjQ5NCAyLjQ5NC0yLjM2NSA2LjMzYy0zLjg5NiAzLjA3Ny0uODg5IDQuODMyIDAgNi44MzYtMi4yNzQtMi4wNTMtMy45NDMtMy44NTgtMi44MjQtNS41NCAxLjY0NC0yLjQ2OCA2LjE5Ny0zLjY2NSA1LjE4OS03LjYyNk05LjczNCAyMy45MjRjNC4zMjIuMjc3IDEwLjk1OS0uMTU0IDExLjExNi0yLjE5OCAwIDAtLjMwMi43NzUtMy41NzIgMS4zOTEtMy42ODguNjk0LTguMjM5LjYxMy0xMC45MzcuMTY4IDAgMCAuNTUzLjQ1NyAzLjM5My42MzkiLz48L3N2Zz4=&logoColor=white" alt="Java" />
  </a>
  &nbsp;
  <a href="https://liubang.github.io/playground/go/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/go/badge.json&style=flat-square&logo=go&logoColor=white" alt="Go" />
  </a>
  &nbsp;
  <a href="https://github.com/liubang/playground/actions/workflows/build_python.yml">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/python/badge.json&style=flat-square&logo=python&logoColor=white" alt="Python" />
  </a>
</p>

这是一个个人技术实验 Monorepo，内容集中在分布式系统、存储引擎、编译器与解释器、模板元编程等方向。仓库使用 [Bazel](https://bazel.build/) 统一管理 C++、Java、Go 和 Python 项目的构建与测试。

## 主要项目

| 项目 | 说明 |
| --- | --- |
| [MiniDFS](cpp/pl/minidfs/) | 类 HDFS 的分布式文件系统，包含 NameNode、DataNode 和 Client，支持块存储、副本管理、心跳与块汇报、MySQL 元数据存储及 Docker 部署。 |
| [Flux](cpp/pl/flux/) | Flux 查询语言子集解释器，覆盖词法分析、语法分析、AST、语义分析、规则与代价优化、物理执行、SQLite/MySQL Connector，并提供 LSP 和 REPL。 |
| [SSTable](cpp/pl/sst/) | LSM-Tree 存储引擎组件，包含 Block 编解码、布隆过滤器、zstd/snappy 压缩、迭代器、版本管理和 CLI 工具。 |
| [Braft Counter](cpp/pl/braft/) | 基于 braft 的 Raft 状态机示例，演示日志复制、快照、Leader 选举和集群部署。 |
| [Meta](cpp/meta/) | C++20 模板元编程实验，包括 Type List、Expression Template、Pattern Matching 和 Tuple Iteration。 |
| [Recall](cpp/pl/recall/) | 基于 FAISS 的向量召回服务，提供 gRPC 接口。 |

此外，仓库还包含 [Skip List](cpp/pl/skiplist/)、[Bloom Filter](cpp/pl/bloom/)、[Arena Allocator](cpp/pl/arena/)、[Thread Pool](cpp/pl/thread/)、[Geohash](cpp/pl/geohash/)、[Brainfuck Interpreter](cpp/pl/bf/) 和 [HTTP Server](cpp/pl/http/) 等小型实现。

## 仓库结构

| 目录 | 说明 |
| --- | --- |
| `cpp/` | C++20 项目，包括分布式系统、存储引擎、查询语言、模板元编程和 RPC 示例。 |
| `java/` | Java 21 项目，包括 Spring Boot 3.5 示例。 |
| `go/` | Go 1.24 项目，包括工具库和多种 cgo 调用方式。 |
| `python/` | Python 3.13 项目，包括 pybind11 绑定和 Manim 动画。 |
| `proto/` | 跨语言共享的 Protobuf 定义。 |
| `tla/` | TLA+ 形式化规约。 |
| `registry/` | Bazel 本地模块注册表，包括 OpenBLAS、ISA-L 等模块。 |
| `php/` | PHP Router 示例。 |
| `bash/` | Shell 脚本示例。 |

## 构建

本地需要安装 Bazelisk 和 C++ 编译器。JDK 21、Go 1.24 和 Python 3.13 工具链由 Bazel 自动下载。

```bash
# macOS
brew install bazelisk llvm libomp

# Ubuntu/Debian
sudo apt-get install -y gcc-14 g++-14 nasm
curl -L https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o /usr/local/bin/bazel
chmod +x /usr/local/bin/bazel
```

常用命令：

```bash
# 全量构建与测试
bazel build //...
bazel test //...

# 按语言构建
bazel build //cpp/...
bazel build //java/...
bazel build //go/...
bazel build //python/...

# C++ 构建配置
bazel build //cpp/... --config=llvm
bazel build //cpp/... --config=release
```

项目通过 `.bazelversion` 锁定 Bazel `8.7.0`。开发构建默认开启 AddressSanitizer；使用 `--config=release` 时关闭。

Java Maven 依赖变更后，运行以下命令重新生成锁文件：

```bash
REPIN=1 bazel run @maven//:pin
```

## Bazel 配置

Bazel 负责维护跨语言依赖图和工具链配置：

- 使用统一的 `BUILD` 文件描述 C++、Java、Go 和 Python 目标。
- 从共享 Protobuf 定义生成各语言代码。
- 自动下载 Java、Go 和 Python 工具链。
- 通过内容哈希缓存减少重复构建。
- 使用 `.bazelversion`、`MODULE.bazel.lock` 和 `maven_install.json` 固定构建依赖。

## 覆盖率

CI 自动生成覆盖率报告：

- [C++](https://liubang.github.io/playground/cpp/)
- [Java](https://liubang.github.io/playground/java/)
- [Go](https://liubang.github.io/playground/go/)

## 许可证

本项目使用 [Apache License 2.0](./LICENSE)。
