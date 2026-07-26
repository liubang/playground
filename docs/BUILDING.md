# 构建手册

本文是仓库的完整构建参考，覆盖各平台前置依赖、构建配置、Maven/Docker E2E 与工具链版本。日常使用的最小命令集见根 [README](../README.md) 构建一节。

## 前置依赖

以下工具需要手动安装，其余依赖（JDK 21、Go SDK 1.26.4、Python 3.13 工具链及所有第三方 C++ 库）由 Bazel 自动下载。

| 依赖                                               | 最低版本             | 说明                                                          |
| -------------------------------------------------- | -------------------- | ------------------------------------------------------------- |
| [Bazelisk](https://github.com/bazelbuild/bazelisk) | —                    | Bazel 版本管理器，项目通过 .bazelversion 锁定 Bazel 8.7.0     |
| C++ 编译器                                         | Clang 16+ 或 GCC 13+ | 需支持 C++20；macOS 推荐 Homebrew LLVM，Linux 推荐 GCC-14     |
| nasm                                               | 2.15+                | 仅 Linux，ISA-L 汇编优化需要                                  |
| libomp                                             | —                    | OpenMP 支持，macOS 和 Linux 均需要                            |
| pkg-config                                         | —                    | 仅 macOS                                                      |
| Docker + Compose                                   | Docker 24+           | 仅容器实验环境及 MiniDFS/Big Data E2E 需要                    |

## macOS 安装

```bash
brew install bazelisk llvm libomp pkg-config
```

Homebrew LLVM 安装后，.bazelrc 中的 macos 配置会自动使用 /opt/homebrew/opt/llvm/bin/clang。最低系统版本要求 macOS 13.3。

## Ubuntu/Debian 安装

```bash
# 编译器与工具
sudo apt-get install -y gcc-14 g++-14 nasm libomp-dev

# Bazelisk
curl -fSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o /usr/local/bin/bazel
sudo chmod +x /usr/local/bin/bazel
```

如需使用 Clang 构建，安装 clang-18 并通过 --action_env=CC=clang-18 指定。

## 构建配置说明

项目通过 .bazelrc 提供以下预设配置：

| 配置             | 说明                                           |
| ---------------- | ---------------------------------------------- |
| 默认             | C++20，开启 AddressSanitizer                   |
| --config=release | 优化构建（-c opt），关闭 ASan                  |
| --config=llvm    | 使用 /opt/app/llvm 下的 Clang + libc++ + lld   |
| --config=gcc     | 使用 GCC + gold 链接器                         |
| --config=macos   | macOS 默认，Homebrew LLVM + libc++             |
| --config=linux   | Linux 默认，等同于 --config=gcc                |

## Java：Bazel 与 Maven

Java 同时支持 Bazel 和 Maven 构建：

```bash
# Maven
cd java && mvn compile && mvn test

# Bazel
bazel build //java/...
```

Maven 依赖变更后，运行以下命令重新生成锁文件：

```bash
REPIN=1 bazel run @maven//:pin
```

## Docker E2E

MiniDFS 可在 Docker 中完成 C++ 单元测试、Linux 镜像构建和三节点集群 E2E：

```bash
./docker/minidfs/tests/e2e.sh all
```

Big Data Java 扩展以 Bazel 为主构建系统。Spark 扩展输出 Java 17 bytecode，Trino Plugin 使用 rules_pkg 生成保留插件目录结构的 ZIP：

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

详细的集群配置和测试说明见 [docker/README.md](../docker/README.md) 与 [docker/bigdata/TESTING.md](../docker/bigdata/TESTING.md)。

## 运行 Echo 服务

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

## Bazel 自动管理的工具链与依赖

以下工具链和库无需手动安装，由 MODULE.bazel 声明并在首次构建时自动下载：

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

完整依赖列表见 [MODULE.bazel](../MODULE.bazel)。构建依赖通过 .bazelversion、MODULE.bazel.lock 和 maven_install.json 固定版本，确保可复现。

## 相关文档

- [cpp/doc/build_gcc.md](../cpp/doc/build_gcc.md)：GCC 工具链细节
- [cpp/doc/build_llvm.md](../cpp/doc/build_llvm.md)：LLVM 工具链细节
