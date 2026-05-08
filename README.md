<h1 align="center">Playground</h1>

<p align="center">
  <a href="https://github.com/liubang/playground/actions/workflows/build_cpp.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_cpp.yml?label=build-cpp&style=flat-square&branch=main" alt="build-cpp" />
  </a>
  <a href="https://github.com/liubang/playground/actions/workflows/build_go.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_go.yml?label=build-go&style=flat-square&branch=main" alt="build-go" />
  </a>
  <a href="https://github.com/liubang/playground/actions/workflows/build_python.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_python.yml?label=build-python&style=flat-square&branch=main" alt="build-python" />
  </a>
</p>

个人多语言实验项目，用于学习、原型验证和小型方案探索。涵盖 C++、Go、Python、PHP、TLA+、LaTeX 等多个技术方向。

## 目录结构

```text
.
├── cpp/        C++ 实验、工具库、测试与笔记
├── go/         Go 示例、小工具与 cgo 示范
├── python/     Python 示例、Bazel targets 与 uv 管理的项目
├── php/        简易 PHP 路由实现
├── proto/      共享 protobuf 定义
├── tla/        TLA+ 规约与生成产物
├── latex/      LaTeX/TikZ 示例
├── registry/   Bazel registry 与 patch 实验
├── bazel/      Bazel 辅助工具与本地 patch
└── bash/       Shell 脚本
```

## 环境依赖

### 基础要求

| 依赖 | 最低版本 | 说明 |
|------|---------|------|
| Bazel | 8.6.0 | 通过 `.bazelversion` 锁定，建议使用 [Bazelisk](https://github.com/bazelbuild/bazelisk) 管理 |
| C++ 标准 | C++20 | 编译标准由 `.bazelrc` 中 `--cxxopt=-std=c++20` 指定 |
| Clang | ≥ 18 | macOS 推荐 Homebrew LLVM；Linux 可用系统 Clang 或 GCC |
| GCC | ≥ 13 | Linux 默认编译器（需支持 C++20） |
| Go | ≥ 1.21 | 由 Bazel rules_go 管理，本地开发需 Go 环境 |
| Python | ≥ 3.11 | 非 Bazel 工作流使用 [uv](https://github.com/astral-sh/uv) 管理 |

### macOS 环境安装

```bash
# Bazelisk
brew install bazelisk

# LLVM 工具链（clang、clangd、lld 等）
brew install llvm

# Go
brew install go

# Python (uv)
brew install uv
```

安装 LLVM 后，确认 `/opt/homebrew/opt/llvm/bin/clang++` 可用。项目 `.bazelrc` 中 `build:macos` 配置已引用此路径。

### Linux 环境安装（Debian/Ubuntu）

```bash
# Bazelisk
wget -qO /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# GCC（默认编译器）
sudo apt install gcc-13 g++-13

# 或使用 LLVM（可选，对应 .bazelrc 中 build:llvm 配置）
# 安装后需将 clang/clang++/lld 放置于 /opt/app/llvm/bin/
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 18

# Go
sudo apt install golang-go

# Python (uv)
curl -LsSf https://astral.sh/uv/install.sh | sh
```

## 构建与测试

```bash
# 全量构建与测试
bazel build //...
bazel test //...

# 按语言构建
bazel build //cpp/...
bazel build //go/...
bazel build //python/...

# 生成 compile_commands.json（供 clangd 使用）
bazel run @hedron_compile_commands//:refresh_all
```

macOS 下默认使用 Homebrew LLVM 工具链；Linux 下默认使用 GCC，如需切换至 LLVM：

```bash
bazel build //cpp/... --config=llvm
```

## clangd 配置

项目使用 [Hedron Compile Commands Extractor](https://github.com/hedronvision/bazel-compile-commands-extractor) 生成 `compile_commands.json`，配合项目根目录的 `.clangd` 文件为 clangd 提供正确的编译参数。

### macOS 额外配置

macOS 上若使用 Mason（Neovim）或独立安装的 clangd，其内置的 `resource-dir` 和 libc++ 头文件可能与 Homebrew LLVM 版本不匹配，导致模板实例化错误（如 `no type named 'difference_type' in 'std::allocator_traits'`）。

解决方法：创建 clangd 用户级配置文件 `~/Library/Preferences/clangd/config.yaml`：

```yaml
CompileFlags:
  Compiler: /opt/homebrew/opt/llvm/bin/clang++
  Add:
    - -resource-dir=/opt/homebrew/opt/llvm/lib/clang/22
```

该配置使 clangd 通过 Homebrew 的 clang++ 驱动推导 libc++ 头文件路径，并使用与之匹配的 `resource-dir`，确保标准库头文件版本一致。

> 注意：若 Neovim 的 clangd 启动参数中包含 `--query-driver`，该参数会与上述 `Compiler` 指令冲突，导致多版本头文件混用。macOS 上应移除 `--query-driver`。

### Linux 配置

Linux 上 clangd 通常与系统标准库版本一致，一般无需额外的用户级配置。若使用独立安装的 clangd 且版本与系统 libstdc++ 不匹配，可创建 `~/.config/clangd/config.yaml` 并参照上述 macOS 配置格式指定正确的 `Compiler` 和 `resource-dir` 路径。

## 其他说明

- `python/manim/manimations` 目录下的 Manim 动画通过 uv 运行：`cd python/manim/manimations && uv run manim -pqh hello.py HelloWorld`
- CI 通过 GitHub Actions 验证 C++、Go、Python 三个方向的构建
- 代码按主题组织，侧重实验性和可读性

## 许可证

Apache License 2.0，详见 [LICENSE](./LICENSE)。
