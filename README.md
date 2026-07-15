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

## 为什么是 Monorepo + Bazel

将多语言项目放在同一个仓库中，配合 Bazel 构建系统，带来几个实际好处：

- **统一构建入口**：无论 C++、Java、Go 还是 Python，一条 `bazel build //...` 即可完成全量构建，不需要为每种语言维护独立的构建脚本和工具链。
- **跨语言依赖管理**：共享的 Protobuf 定义可以一次编写，自动生成各语言代码；跨语言调用的接口变更在同一次提交中完成，不会出现版本漂移。
- **确定性与可复现**：Bazel 通过内容哈希缓存和沙箱执行保证构建结果确定，同一份代码在 macOS 和 Linux CI 上产出一致的产物。工具链（JDK、Go、Python）由 Bazel 自动下载，开发者只需安装 Bazelisk 即可上手。
- **增量构建**：依赖图精确到文件级别，修改一个 `.proto` 文件只会重新编译受影响的目标，大幅缩短开发迭代周期。
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
| [Echo Service](proto/echo/)     | 多语言 gRPC Echo 服务示例。共享 proto 定义，C++ / Java / Python / Go 四语言各自实现 server + client，支持跨语言互操作。                | gRPC, protobuf, C++20, Java 21, Python 3.13, Go        |
| [Big Data Lab](docker/bigdata/) | HDFS、Hive、Spark、Trino、Iceberg 与 MySQL 集群；支持从 Java 扩展源码构建到 Docker SQL 断言的一体化 E2E。                              | Spark 4.0.2, Trino 468, Hive 4, Iceberg, Docker        |

此外，仓库还包含 [Skip List](cpp/pl/skiplist/)、[Bloom Filter](cpp/pl/bloom/)、[Arena Allocator](cpp/pl/arena/)、[Thread Pool](cpp/pl/thread/)、[Geohash](cpp/pl/geohash/)、[Brainfuck Interpreter](cpp/pl/bf/) 和 [HTTP Server](cpp/pl/http/) 等小型实现。

## 仓库结构

| 目录        | 说明                                                                                       | 技术栈                                         |
| ----------- | ------------------------------------------------------------------------------------------ | ---------------------------------------------- |
| `cpp/`      | C++20 项目，包括分布式系统、存储引擎、查询语言、模板元编程和 RPC 示例。                    | C++20, Clang/GCC, folly, brpc, Abseil          |
| `java/`     | Java 项目：Spring Boot、gRPC Echo，以及 Spark/Trino 扩展；以 Bazel 为主并支持 Maven 构建。 | Java 17/21/23, Spark, Trino, Spring Boot, gRPC |
| `go/`       | Go 项目：工具库、cgo 示例 + gRPC Echo 服务。                                               | Go 1.25, cgo, gRPC                             |
| `python/`   | Python 项目：pybind11 绑定、Manim 动画 + gRPC Echo 服务。                                  | Python 3.13, pybind11, gRPC                    |
| `proto/`    | 跨语言共享的 Protobuf 定义，含多语言 Echo 服务定义。                                       | protobuf, gRPC                                 |
| `tla/`      | TLA+ 形式化规约。                                                                          | TLA+                                           |
| `registry/` | Bazel 本地模块注册表，包括 OpenBLAS、ISA-L 等模块。                                        | Bazel bzlmod                                   |
| `php/`      | PHP Router 示例。                                                                          | PHP                                            |
| `bash/`     | Shell 脚本示例。                                                                           | Bash                                           |
| `docker/`   | 本地实验集群：MiniDFS、Big Data、Doris、Hermes、Prometheus/Grafana 和 MySQL。              | Docker Compose                                 |

## 构建

### 前置依赖

以下工具需要手动安装，其余依赖（JDK 21、Go 1.24、Python 3.13 工具链及所有第三方 C++ 库）由 Bazel 自动下载。

| 依赖                                               | 最低版本             | 说明                                                          |
| -------------------------------------------------- | -------------------- | ------------------------------------------------------------- |
| [Bazelisk](https://github.com/bazelbuild/bazelisk) | —                    | Bazel 版本管理器，项目通过 `.bazelversion` 锁定 Bazel `8.7.0` |
| C++ 编译器                                         | Clang 16+ 或 GCC 13+ | 需支持 C++20；macOS 推荐 Homebrew LLVM，Linux 推荐 GCC-14     |
| nasm                                               | 2.15+                | 仅 Linux，ISA-L 汇编优化需要                                  |
| libomp                                             | —                    | OpenMP 支持，macOS 和 Linux 均需要                            |
| pkg-config                                         | —                    | 仅 macOS                                                      |
| Docker + Compose                                   | Docker 24+           | 仅容器实验环境及 MiniDFS/Big Data E2E 需要                    |

### macOS 安装

```bash
brew install bazelisk llvm libomp pkg-config
```

Homebrew LLVM 安装后，`.bazelrc` 中的 `--config=macos` 会自动使用 `/opt/homebrew/opt/llvm/bin/clang`。最低系统版本要求 macOS 13.3。

### Ubuntu/Debian 安装

```bash
# 编译器与工具
sudo apt-get install -y gcc-14 g++-14 nasm libomp-dev

# Bazelisk
curl -fSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o /usr/local/bin/bazel
sudo chmod +x /usr/local/bin/bazel
```

如需使用 Clang 构建，安装 `clang-18` 并通过 `--action_env=CC=clang-18` 指定。

### 常用命令

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
bazel build //cpp/... --config=release   # 优化构建，关闭 ASan
bazel build //cpp/... --config=llvm      # 使用自定义 LLVM 路径
```

### 构建配置说明

项目通过 `.bazelrc` 提供以下预设配置：

| 配置               | 说明                                           |
| ------------------ | ---------------------------------------------- |
| 默认               | C++20，开启 AddressSanitizer                   |
| `--config=release` | 优化构建（`-c opt`），关闭 ASan                |
| `--config=llvm`    | 使用 `/opt/app/llvm` 下的 Clang + libc++ + lld |
| `--config=gcc`     | 使用 GCC + gold 链接器                         |
| `--config=macos`   | macOS 默认，Homebrew LLVM + libc++             |
| `--config=linux`   | Linux 默认，等同于 `--config=gcc`              |

Java 同时支持 Bazel 和 Maven 构建：

```bash
# Maven
cd java && mvn compile && mvn test

# Bazel
bazel build //java/...
```

MiniDFS 可在 Docker 中完成 C++ 单元测试、Linux 镜像构建和三节点集群 E2E：

```bash
./docker/minidfs/tests/e2e.sh all
```

Big Data Java 扩展以 Bazel 为主构建系统。Spark 扩展输出 Java 17 bytecode，Trino Plugin 使用 `rules_pkg` 生成保留插件目录结构的 ZIP：

```bash
# 单元测试与发布产物
bazel test //java/pl/bigdata:tests
bazel build \
  //java/pl/bigdata:spark_extension_jar \
  //java/pl/bigdata:trino_plugin_zip

# 构建扩展、启动 Docker 集群、执行 SQL 断言并清理测试产物
./docker/bigdata/tests/e2e.sh all

# 可选：使用 Maven/JDK 23 容器验证备用构建路径
BUILD_SYSTEM=maven ./docker/bigdata/tests/e2e.sh all
```

详细的集群配置和测试说明见 [`docker/README.md`](docker/README.md) 与 [`docker/bigdata/TESTING.md`](docker/bigdata/TESTING.md)。

Maven 依赖变更后，运行以下命令重新生成锁文件：

```bash
REPIN=1 bazel run @maven//:pin
```

### 运行 Echo 服务

跨语言 gRPC Echo 服务支持四语言任意互操作：

```bash
# 启动任意语言的 server（任选一个）
bazel run //cpp/pl/grpc/echo:echo_server    # C++
bazel run //java/pl/grpc/echo:echo_server   # Java
bazel run //python/pl/grpc/echo:echo_server # Python
bazel run //go/pl/grpc/echo/server          # Go

# 用任意语言的 client 连接（可与 server 语言不同）
bazel run //cpp/pl/grpc/echo:echo_client    # C++
bazel run //java/pl/grpc/echo:echo_client   # Java
bazel run //python/pl/grpc/echo:echo_client # Python
bazel run //go/pl/grpc/echo/client          # Go
```

### Bazel 自动管理的工具链与依赖

以下工具链和库无需手动安装，由 `MODULE.bazel` 声明并在首次构建时自动下载：

| 类别          | 版本               |
| ------------- | ------------------ |
| JDK           | 21                 |
| Go SDK        | 1.26.4             |
| Python        | 3.13               |
| protobuf      | 31.1               |
| gRPC (C++)    | 1.74.1             |
| gRPC (Java)   | 1.74.0             |
| gRPC (Go)     | 1.81.1             |
| Abseil C++    | 20250127.1         |
| folly         | 2025.01.13         |
| brpc          | 1.16.0             |
| Boost         | 1.90.0             |
| FAISS         | 1.14.1             |
| fmt           | 12.1.0             |
| zstd / snappy | 1.5.6 / 1.2.1      |
| GoogleTest    | 1.17               |
| Spark         | 4.0.2 / Scala 2.13 |
| Trino SPI     | 468                |
| rules_pkg     | 1.1.0              |

完整依赖列表见 [`MODULE.bazel`](MODULE.bazel)。构建依赖通过 `.bazelversion`、`MODULE.bazel.lock` 和 `maven_install.json` 固定版本，确保可复现。

## 覆盖率

CI 自动生成覆盖率报告：

- [C++](https://liubang.github.io/playground/cpp/)
- [Java](https://liubang.github.io/playground/java/)
- [Go](https://liubang.github.io/playground/go/)

## 许可证

本项目使用 [Apache License 2.0](./LICENSE)。
