# Playground

Personal multi-language playground for learning, prototyping, and validating small ideas. The repository is intentionally broad: it collects focused experiments, utility implementations, parser demos, build-system exercises, and supporting notes across several ecosystems.

## Overview

This is not a single product or framework. It is a working lab used to explore:

- C++ language features, data structures, systems utilities, parsers, networking, storage, and benchmarking
- Go tooling and cgo interoperability
- Python experiments around protobuf, pybind, crawling, and Manim animations
- PHP router basics
- TLA+ specifications and LaTeX/TikZ notes
- Bazel module and registry-related experiments

Most code is organized as self-contained examples. Some parts are polished utilities, while others are lightweight testbeds kept for reference.

## Repository Layout

```text
.
├── cpp/        C++ experiments, utilities, tests, and notes
├── go/         Go examples, small tools, and cgo demos
├── python/     Python samples, Bazel targets, and uv-managed projects
├── php/        Minimal PHP router implementation
├── proto/      Shared protobuf definitions
├── tla/        TLA+ specifications and generated PDFs
├── latex/      LaTeX/TikZ examples
├── registry/   Bazel registry and patch experiments
├── bazel/      Bazel helpers and local patches
└── bash/       Small shell scripts
```

### Notable Areas

- `cpp/meta`: template metaprogramming utilities with unit tests
- `cpp/features`: targeted C++17 and C++20 feature experiments
- `cpp/pl`: a larger collection of systems-oriented building blocks, including status handling, storage structures, parsers, HTTP/grpc demos, coroutine experiments, Unicode utilities, logging, threading, and more
- `go/cgo`: cgo interop examples with tests
- `go/tools`: small CLI-oriented utilities
- `python/test_pb`: protobuf-related Python examples
- `python/test_pybind`: Python bindings backed by C++
- `python/test_crawler`: crawler experiments
- `python/manim/manimations`: Manim animation demos

## Build and Test

The repository is primarily driven by Bazel with Bzlmod.

```bash
bazel build //...
bazel test //...
```

Common scoped commands:

```bash
bazel build //cpp/...
bazel test //cpp/...

bazel build //go/...
bazel test //go/...

bazel build //python/...
bazel test //python/...
```

To generate `compile_commands.json` for C/C++ tooling:

```bash
bazel run //:refresh_compile_commands
```

## Environment Notes

- Bazel is the default entry point for most code in this repository
- `python/` also includes a `uv`-managed environment for non-Bazel workflows
- `go/` uses Go modules in addition to Bazel rules
- CI currently validates the C++, Go, and Python areas through GitHub Actions

Example Manim commands:

```bash
cd python/manim/manimations
uv run manim -pqh hello.py HelloWorld
uv run manim -pqh lsm.py LevelDBWriteAndCompaction
```

## Conventions

- Code is grouped by topic rather than by release readiness
- Examples may favor clarity and experimentation over API stability
- Some directories contain design notes, generated artifacts, or partial implementations kept for study

## License

This repository is licensed under the Apache License 2.0. See [LICENSE](./LICENSE).
