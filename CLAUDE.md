# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is a Bazel monorepo. Bazel 8.7.0 is locked via `.bazelversion`. Use `bazel` (not `cmake`/`make`/bazelisk directly — `bazel` invokes the version manager).

```bash
# Full build and test
bazel build //...
bazel test //...

# Per-language
bazel build //cpp/...       # C++ (default: C++20 + ASan, debug mode)
bazel build //java/...      # Java 21 / Spring Boot
bazel build //go/...        # Go 1.24
bazel build //python/...    # Python 3.13

# C++ configs
bazel build //cpp/... --config=release   # -c opt, no ASan
bazel build //cpp/... --config=llvm      # Custom LLVM at /opt/app/llvm

# Single test (Bazel label pattern)
bazel test //cpp/pl/bloom:bloom_filter_test
bazel test //cpp/pl/sstv2:block_test --test_output=all
bazel test //cpp/pl/sstv2/...            # all tests under a package

# CI: cached test results, errors-only output
bazel test //cpp/... --config=release --config=ci

# Compile commands (for clangd/LSP)
bazel run :refresh_compile_commands

# Java Maven dependency lock
REPIN=1 bazel run @maven//:pin

# Format C++ code
./format.sh
```

Tests always run with `-c opt` (see `.bazelrc`). Locally, tests re-run every time (`--nocache_test_results`); CI caches test results (`--config=ci`).

## Architecture

### Language layout
```
cpp/pl/          C++ projects — distributed systems, storage engines, query languages
cpp/meta/        Template metaprogramming (type lists, expression templates, pattern matching)
cpp/features/    C++17/20 language feature experiments
cpp/algo/        Algorithms
java/pl/         Spring Boot services
go/              Go utilities, cgo examples
python/          pybind11 bindings, Manim animations, crawlers
proto/           Shared Protobuf definitions
tla/             TLA+ formal specifications
registry/        Bazel local module registry for patched BCR modules
bazel/           Build patches for third-party deps (brpc, braft, faiss)
```

### C++ conventions
- **Standard:** C++20
- **Build macros:** Every C++ target uses `COPTS`/`LINKOPTS`/`TEST_COPTS` from `//cpp:copts/configure_copts.bzl`, which selects compiler-specific warning flags and conditionally adds ASan flags based on the `//cpp:asan` bool flag (default: True).
- **Headers-only libs:** Libraries like `cpp/meta` expose `hdrs` only via `cc_library(name = "meta", hdrs = glob(["*.h"]), ...)`.
- **Test libs:** Tests use `cc_test` with `TEST_COPTS`, deps on `@googletest//:gtest_main`. Tests live alongside sources or under `ut/` subdirs.
- **Format:** clang-format via `.clang-format` (4-space indent, 100-char column limit, C++20).
- **clangd:** Configured in `.clangd` — strict unused includes, modernize+performance clang-tidy checks. Run `bazel run :refresh_compile_commands` to regenerate `compile_commands.json`.

### Key C++ subprojects
| Path | Description |
|------|-------------|
| `cpp/pl/minidfs/` | HDFS-like distributed FS (NameNode, DataNode, Client, brpc) |
| `cpp/pl/flux/` | Flux query language interpreter (lex/parse/optimize/execute) |
| `cpp/pl/sst/` | LSM-Tree SSTable — block codec, bloom, compression, iterators |
| `cpp/pl/sstv2/` | SSTable v2 rewrite — modular: `block/`, `bloom/`, `codec/`, `compress/`, `index/`, `pattern/`, `types/`, `format/`, `file/` |
| `cpp/pl/braft/` | Raft state machine example using braft |
| `cpp/pl/recall/` | FAISS vector recall with gRPC |
| `cpp/meta/` | Template metaprogramming: type lists, expression templates, pattern matching, tuple iteration |

### Third-party deps
All deps declared in `MODULE.bazel` via bzlmod. Key deps: Abseil, brpc/braft, protobuf/grpc, folly, FAISS, Boost (modular), zstd/snappy, GoogleTest, fmt, simdjson. Some deps (brpc, braft, faiss) carry patches in `bazel/` for Bazel 8 compatibility.

### CI
GitHub Actions in `.github/workflows/`:
- `build_cpp.yml` — macOS (Apple Clang) + Linux (GCC-14, Clang), ASan on
- `build_java.yml`, `build_go.yml`, `build_python.yml` — per-language

### Bazel registry
`registry/` holds `MODULE.bazel` stubs for modules not in BCR: `colm`, `isa-l`, `lemon`, `leveldb`, `nanobench`, `openblas`, `ragel`. Referenced via `--registry=file://%workspace%/registry` in `.bazelrc`.
