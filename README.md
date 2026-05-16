<h1 align="center">Playground</h1>

<p align="center">
  <a href="https://github.com/liubang/playground/actions/workflows/build_cpp.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_cpp.yml?label=build-cpp&style=flat-square&branch=main" alt="build-cpp" />
  </a>
  <a href="https://github.com/liubang/playground/actions/workflows/build_java.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_java.yml?label=build-java&style=flat-square&branch=main" alt="build-java" />
  </a>
  <a href="https://github.com/liubang/playground/actions/workflows/build_go.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_go.yml?label=build-go&style=flat-square&branch=main" alt="build-go" />
  </a>
  <a href="https://github.com/liubang/playground/actions/workflows/build_python.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_python.yml?label=build-python&style=flat-square&branch=main" alt="build-python" />
  </a>
  <a href="https://liubang.github.io/playground/cpp/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/cpp/badge.json&style=flat-square" alt="coverage-cpp" />
  </a>
  <a href="https://liubang.github.io/playground/java/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/java/badge.json&style=flat-square" alt="coverage-java" />
  </a>
  <a href="https://liubang.github.io/playground/go/">
    <img src="https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/liubang/playground/coverage/go/badge.json&style=flat-square" alt="coverage-go" />
  </a>
</p>

个人多语言实验项目，使用 [Bazel](https://bazel.build/) 统一构建，用于学习、原型验证和小型方案探索。涵盖 C++、Java、Go、Python 等多个技术方向，所有语言共享同一个构建系统和 CI 流水线。

## 目录结构

```text
.
├── cpp/        C++ 实验、工具库与测试（C++20，gRPC/brpc/Faiss/RocksDB 等）
├── java/       Java 实验（Spring Boot 3.5，Bazel 原生构建）
├── go/         Go 示例、小工具与 cgo 示范
├── python/     Python 示例与 Bazel targets
├── proto/      共享 protobuf 定义
├── registry/   Bazel 本地 registry（OpenBLAS、ISA-L 等自定义模块）
├── bazel/      Bazel 辅助 patch 文件
├── php/        简易 PHP 路由实现
├── tla/        TLA+ 规约
├── latex/      LaTeX/TikZ 示例
└── bash/       Shell 脚本
```

## 环境依赖

所有语言的工具链和第三方库均由 Bazel 自动管理，本地只需安装 Bazel 本身和 C++ 编译器。Java SDK、Go SDK、Python 解释器、Maven 依赖等都由 Bazel 在首次构建时自动下载，无需手动安装。

### macOS

```bash
brew install bazelisk llvm libomp
```

安装后确认 `/opt/homebrew/opt/llvm/bin/clang++` 可用，`.bazelrc` 中 `build:macos` 已引用此路径。

### Linux（Debian/Ubuntu）

```bash
# Bazelisk
wget -qO /usr/local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# GCC（默认编译器，需支持 C++20）
sudo apt install gcc-14 g++-14

# 构建 C++ 项目所需的系统依赖
sudo apt install nasm libomp-dev
```

如需使用 LLVM 替代 GCC，安装 Clang 18+ 后以 `--config=llvm` 构建即可。

## 构建与测试

Bazel 版本通过 `.bazelversion` 锁定为 8.6.0，Bazelisk 会自动下载对应版本。

### 全量构建

```bash
bazel build //...
bazel test //...
```

### C++

C++ 标准为 C++20，macOS 默认使用 Homebrew LLVM，Linux 默认使用 GCC。

```bash
bazel build //cpp/...
bazel test //cpp/...

# Linux 切换至 LLVM 工具链
bazel build //cpp/... --config=llvm

# Release 模式（关闭 ASan）
bazel build //cpp/... --config=release
```

### Java

Java 语言版本和运行时均为 21，由 Bazel 通过 `rules_java` 自动下载远程 JDK，无需本地安装。Maven 依赖通过 `rules_jvm_external` 管理，依赖声明在 `MODULE.bazel` 中，解析结果锁定在 `maven_install.json`。

```bash
bazel build //java/...
bazel test //java/...

# 构建 Spring Boot fat jar（可通过 java -jar 直接运行）
bazel build //java/t/spring:demo_deploy.jar

# 直接启动 Spring Boot 应用
bazel run //java/t/spring:demo
```

修改 `MODULE.bazel` 中的 Maven 依赖后，需要重新生成锁文件：

```bash
REPIN=1 bazel run @maven//:pin
```

### Go

Go SDK 由 `rules_go` 自动下载（当前版本 1.24.12），第三方依赖通过 Gazelle 从 `go/go.mod` 解析，无需本地安装 Go。

```bash
bazel build //go/...
bazel test //go/...
```

如需在 Bazel 之外使用 Go 工具链（如 `go mod tidy`），需本地安装 Go：macOS 上 `brew install go`，Linux 上 `sudo apt install golang-go` 或从官网下载。

### Python

Python 解释器由 `rules_python` 自动下载（当前版本 3.13），pip 依赖从 `requirements_lock.txt` 解析。

```bash
bazel build //python/...
bazel test //python/...
```

`python/manim/manimations` 目录下的 Manim 动画不走 Bazel，通过 [uv](https://github.com/astral-sh/uv) 运行：

```bash
cd python/manim/manimations && uv run manim -pqh hello.py HelloWorld
```

## 覆盖率

CI 自动生成 C++、Java、Go 的测试覆盖率报告，发布在 GitHub Pages 上：

- C++ 覆盖率详情：https://liubang.github.io/playground/cpp/
- Java 覆盖率详情：https://liubang.github.io/playground/java/
- Go 覆盖率详情：https://liubang.github.io/playground/go/

## clangd 配置

项目使用 [Hedron Compile Commands Extractor](https://github.com/hedronvision/bazel-compile-commands-extractor) 生成 `compile_commands.json`，配合根目录的 `.clangd` 文件为 clangd 提供编译参数：

```bash
bazel run @hedron_compile_commands//:refresh_all
```

macOS 上若 clangd 的 libc++ 头文件与 Homebrew LLVM 版本不匹配，创建 `~/Library/Preferences/clangd/config.yaml`：

```yaml
CompileFlags:
  Compiler: /opt/homebrew/opt/llvm/bin/clang++
  Add:
    - -resource-dir=/opt/homebrew/opt/llvm/lib/clang/22
```

> 注意：Neovim 的 clangd 启动参数中若包含 `--query-driver`，会与上述 `Compiler` 指令冲突，macOS 上应移除。

Linux 上 clangd 通常与系统标准库版本一致，一般无需额外配置。

## 许可证

Apache License 2.0，详见 [LICENSE](./LICENSE)。
