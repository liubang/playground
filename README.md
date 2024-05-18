<h1 align="center">Personal Learning Playground</h1>

<div align="center">
<p>
    <a href="https://github.com/liubang/playground/actions/workflows/build_cpp.yml">
        <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_cpp.yml?label=build-cpp&style=flat-square&branch=main" alt="build-cpp" />
    </a>
    <a href="https://github.com/liubang/playground/actions/workflows/build_go.yml">
        <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_go.yml?label=build-go&style=flat-square&branch=main" alt="build-go" />
    </a>
    <a href="https://github.com/liubang/playground/actions/workflows/build_java.yml">
        <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_java.yml?label=build-java&style=flat-square&branch=main" alt="build-java" />
    </a>
    <a href="https://github.com/liubang/playground/actions/workflows/build_python.yml">
        <img src="https://img.shields.io/github/actions/workflow/status/liubang/playground/build_python.yml?label=build-python&style=flat-square&branch=main" alt="build-python" />
    </a>
</p>
</div>

### 运行

```bash
bazel build //...
bazel test //...
```

### 更新 C/C++ compile_commands.json

```bash
bazel run //:refresh_compile_commands
```

### 更新 java 依赖

```bash
REPIN=1 bazel run @unpinned_maven//:pin
```

### 更新 rust 依赖

```bash
CARGO_BAZEL_REPIN=1 bazel sync --only=crate_index
```
