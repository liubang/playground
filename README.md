<h1 align="center">Playground</h1>

<p align="center">
  <a href="https://liubang.github.io/playground/cpp/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/cpp/badge.json&style=flat-square&logo=cplusplus&logoColor=white" alt="C++" />
  </a>
  &nbsp;
  <a href="https://liubang.github.io/playground/go/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/go/badge.json&style=flat-square&logo=go&logoColor=white" alt="Go" />
  </a>
  &nbsp;
  <a href="https://liubang.github.io/playground/java/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/java/badge.json&style=flat-square&logo=data:image/svg%2Bxml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0id2hpdGUiIGQ9Ik04Ljg1MSAxOC41NnMtLjkxNy41MzQuNjUzLjcxNGMxLjkwMi4yMTggMi44NzQuMTg3IDQuOTY5LS4yMTEgMCAwIC41NTIuMzQ2IDEuMzIxLjY0Ni00LjY5OSAyLjAxMy0xMC42MzMtLjExOC02Ljk0My0xLjE0OW0tLjU3NS0yLjYyN3MtMS4wMjguNzYyLjU0My45MjRjMi4wMzIuMjEgMy42MzYuMjI3IDYuNDEzLS4zMDggMCAwIC4zODQuMzg5Ljk4Ny42MDItNS42NzkgMS42NjEtMTIuMDA3LjEzLTcuOTQzLTEuMjE4bTQuODQtNC40NThjMS4xNTggMS4zMzMtLjMwNCAyLjUzMy0uMzA0IDIuNTMzczIuOTM5LTEuNTIgMS41ODktMy40MThjLTEuMjYxLTEuNzcyLTIuMjI4LTIuNjUyIDMuMDA3LTUuNjg4LS4wMDEgMC04LjIxNiAyLjA1MS00LjI5MiA2LjU3M00xOS4xMTYgMjAuOTU4cy42NzkuNTU5LS43NDcuOTkyYy0yLjcxMi44MjItMTEuMjg4IDEuMDY5LTEzLjY2OS4wMzMtLjg1Ni0uMzczLjc1LS44OSAxLjI1NC0uOTk5LjUyNy0uMTE0LjgyOC0uMDkzLjgyOC0uMDkzLS45NTMtLjY3MS02LjE1NiAxLjMxNy0yLjY0MyAxLjg4NyA5LjU4IDEuNTUzIDE3LjQ2Mi0uNyAxNC45NzctMS44Mk05LjI5MiAxMy4yMXMtNC4zNjIgMS4wMzYtMS41NDQgMS40MTJjMS4xODkuMTU5IDMuNTYxLjEyMyA1Ljc3LS4wNjMgMS44MDYtLjE1MiAzLjYxOC0uNDc3IDMuNjE4LS40NzdzLS42MzcuMjcyLTEuMDk4LjU4N2MtNC40MjkgMS4xNjUtMTIuOTg2LjYyMy0xMC41MjItLjU2OSAyLjA4Mi0xLjAwNSAzLjc3Ni0uODkgMy43NzYtLjg5bTcuODI0IDQuMzc0YzQuNTAzLTIuMzQgMi40MjEtNC41ODkuOTY4LTQuMjg1LS4zNTUuMDc0LS41MTUuMTM4LS41MTUuMTM4cy4xMzItLjIwNy4zODUtLjI5N2MyLjg3NS0xLjAxMSA1LjA4NiAyLjk4MS0uOTI4IDQuNTYyIDAgMCAuMDctLjA2Mi4wOS0uMTE4TTE0LjQwMS4yOXMyLjQ5NCAyLjQ5NC0yLjM2NSA2LjMzYy0zLjg5NiAzLjA3Ny0uODg5IDQuODMyIDAgNi44MzYtMi4yNzQtMi4wNTMtMy45NDMtMy44NTgtMi44MjQtNS41NCAxLjY0NC0yLjQ2OCA2LjE5Ny0zLjY2NSA1LjE4OS03LjYyNk05LjczNCAyMy45MjRjNC4zMjIuMjc3IDEwLjk1OS0uMTU0IDExLjExNi0yLjE5OCAwIDAtLjMwMi43NzUtMy41NzIgMS4zOTEtMy42ODguNjk0LTguMjM5LjYxMy0xMC45MzcuMTY4IDAgMCAuNTUzLjQ1NyAzLjM5My42MzkiLz48L3N2Zz4=&logoColor=white" alt="Java" />
  </a>
  &nbsp;
  <a href="https://github.com/liubang/playground/actions/workflows/build_python.yml">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/python/badge.json&style=flat-square&logo=python&logoColor=white" alt="Python" />
  </a>
</p>

这是一个个人技术实验 Monorepo，内容集中在分布式系统、存储引擎、编译器与解释器、模板元编程、Coding Agent 等方向。仓库使用 [Bazel](https://bazel.build/) 统一管理 C++、Go、Java 和 Python 项目的构建与测试。

## 目录

- [为什么是 Monorepo + Bazel](#为什么是-monorepo--bazel)
- [主要项目](#主要项目)
- [仓库结构](#仓库结构)
- [构建](#构建)
- [覆盖率](#覆盖率)
- [许可证](#许可证)

## 为什么是 Monorepo + Bazel

将多语言项目放在同一个仓库中，配合 Bazel 构建系统，带来几个实际好处：

- **统一构建入口**：无论 C++、Go、Java 还是 Python，一条命令即可完成全量构建与测试，不需要为每种语言维护独立的构建脚本和工具链。
- **跨语言依赖管理**：共享的 Protobuf 定义可以一次编写，自动生成各语言代码；跨语言调用的接口变更在同一次提交中完成，不会出现版本漂移。
- **确定性与可复现**：Bazel 通过内容哈希缓存和沙箱执行保证构建过程可复现，同一份代码在 macOS 和 Linux CI 上行为一致。工具链（Go SDK、JDK、Python）由 Bazel 自动下载，开发者只需安装 Bazelisk 即可上手。
- **增量构建**：依赖图精确到文件级别，修改一个 proto 文件只会重新编译受影响的目标，大幅缩短开发迭代周期。
- **多平台 CI**：当前 CI 覆盖 Linux (GCC-14 / Clang) 和 macOS (Apple Clang) 三个矩阵，确保代码在不同编译器和操作系统上行为一致。

> "Almost every new project benefits from incorporating an artifact-based build system like Bazel right from the start."
>
> "The monorepo approach has some inherent benefits, and chief among them is that adhering to One Version is trivial."
>
> — _Software Engineering at Google_, Chapter 16 & 18

更多讨论见 [Chapter 16: Version Control and Branch Management](https://abseil.io/resources/swe-book/html/ch16.html) 和 [Chapter 18: Build Systems and Build Philosophy](https://abseil.io/resources/swe-book/html/ch18.html)。

## 主要项目

| 项目                            | 说明                                                                                                                                   | 技术栈                                                 |
| ------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------ |
| [MiniDFS](cpp/pl/minidfs/)      | 类 HDFS 的分布式文件系统，包含 NameNode、DataNode 和 Client，支持块存储、副本管理、心跳与块汇报、MySQL 元数据存储及 Docker 部署。      | C++20, brpc, protobuf, Boost.MySQL, ISA-L/crc32c, zstd |
| [Flux](cpp/pl/flux/)            | Flux 查询语言子集解释器，覆盖词法分析、语法分析、AST、语义分析、规则与代价优化、物理执行、SQLite/MySQL Connector，并提供 LSP 和 REPL。 | C++20, Abseil, simdjson, SQLite, MySQL                 |
| [SSTable](cpp/pl/sst/)          | LSM-Tree 存储引擎组件，包含 Block 编解码、布隆过滤器、zstd/snappy 压缩、迭代器、版本管理和 CLI 工具。                                  | C++20, zstd, snappy                                    |
| [SSTable v2](cpp/pl/sstv2/)     | SSTable 完全重写版本。模块化架构：类型化键系统（C++20 concept）、memcomparable 编码、多级索引树、列存储 Block、布隆过滤器。            | C++20, Abseil, zstd, snappy, lz4, xxHash               |
| [Braft Counter](cpp/pl/braft/)  | 基于 braft 的 Raft 状态机示例，演示日志复制、快照、Leader 选举和集群部署。                                                             | C++20, braft, brpc, protobuf                           |
| [Meta](cpp/meta/)               | C++20 模板元编程实验，包括 Type List、Expression Template、Pattern Matching 和 Tuple Iteration。                                       | C++20                                                  |
| [Recall](cpp/pl/recall/)        | 基于 FAISS 的向量召回服务，提供 gRPC 接口。                                                                                            | C++20, FAISS, OpenBLAS, gRPC, protobuf                 |
| [Loom](go/pl/loom/)             | 生产级 Coding Agent：交互式 TUI、事件溯源会话与崩溃恢复、工具沙箱与分级审批、Skills 扩展、OTel/Langfuse 观测；Server 模式设计中。      | Go, Bubble Tea, SQLite, OpenTelemetry                  |
| [Echo Service](proto/echo/)     | 多语言 gRPC Echo 服务示例。共享 proto 定义，C++ / Go / Java / Python 四语言各自实现 server + client，支持跨语言互操作。                | gRPC, protobuf, C++20, Go, Java 21, Python 3.13        |
| [Big Data Lab](docker/bigdata/) | HDFS、Hive、Spark、Trino、Iceberg 与 MySQL 集群；支持从 Java 扩展源码构建到 Docker SQL 断言的一体化 E2E。                              | Spark 4.0.2, Trino 468, Hive 4, Iceberg, Docker        |

此外，仓库还包含 [Skip List](cpp/pl/skiplist/)、[Bloom Filter](cpp/pl/bloom/)、[Arena Allocator](cpp/pl/arena/)、[Thread Pool](cpp/pl/thread/)、[Geohash](cpp/pl/geohash/)、[Brainfuck Interpreter](cpp/pl/bf/) 和 [HTTP Server](cpp/pl/http/) 等小型实现。

## 仓库结构

| 目录        | 说明                                                                       |
| ----------- | -------------------------------------------------------------------------- |
| cpp/        | C++20 项目：分布式系统、存储引擎、查询语言、模板元编程（见主要项目表）。   |
| go/         | Go 项目：Loom Coding Agent、工具库、cgo 与 gRPC Echo 示例。                |
| java/       | Java 项目：Spring Boot、gRPC Echo、Spark/Trino 扩展（Bazel + Maven）。     |
| python/     | Python 项目：pybind11 绑定、Manim 动画、gRPC Echo 服务。                   |
| proto/      | 跨语言共享的 Protobuf 定义。                                               |
| docker/     | 本地实验集群：MiniDFS、Big Data、Doris、Hermes、监控与 MySQL。             |
| registry/   | Bazel 本地模块注册表（OpenBLAS、ISA-L 等）。                               |
| 其他        | tla/（TLA+ 形式化规约）、latex/（TikZ 示例）、php/（Router）、bash/。      |

## 构建

前置依赖：

- 全平台需要 [Bazelisk](https://github.com/bazelbuild/bazelisk)（仓库通过 .bazelversion 锁定 Bazel 8.7.0）和 C++20 编译器（Clang 16+ 或 GCC 13+）。
- Go SDK 1.26.4、JDK 21、Python 3.13 工具链及全部第三方依赖由 Bazel 在首次构建时自动下载。
- Docker 仅容器实验环境与 MiniDFS/Big Data E2E 需要。

```bash
# macOS（最低 macOS 13.3）
brew install bazelisk llvm libomp pkg-config

# Ubuntu/Debian
sudo apt-get install -y gcc-14 g++-14 nasm libomp-dev
```

常用命令：

```bash
bazel build //...                        # 全量构建
bazel test //...                         # 全量测试
bazel test //cpp/pl/sstv2/...            # 单包测试
bazel build //cpp/... --config=release   # 优化构建（关闭 ASan）
bazel run :refresh_compile_commands      # 生成 compile_commands.json（clangd）
bazel run //:format                      # 格式化全部代码（C++/Go/Java/Python/...）
```

平台配置、构建选项、Maven 与 Docker E2E、工具链版本清单等完整内容见 [docs/BUILDING.md](docs/BUILDING.md)。

## 覆盖率

CI 自动生成覆盖率报告：

- [C++](https://liubang.github.io/playground/cpp/)
- [Go](https://liubang.github.io/playground/go/)
- [Java](https://liubang.github.io/playground/java/)

Python 暂未发布覆盖率页面，其 badge 链接至 [build_python](https://github.com/liubang/playground/actions/workflows/build_python.yml) workflow。

## 许可证

本项目使用 [Apache License 2.0](./LICENSE)。
