load("@rules_python//python:defs.bzl", "py_test")

py_test(
    name = "migrate_memory_v12_test",
    srcs = [
        "migrate_memory_v12.py",
        "migrate_memory_v12_test.py",
    ],
    data = ["//go/pl/loom/internal/session:sqlite_store.go"],
    imports = ["."],
    main = "migrate_memory_v12_test.py",
    python_version = "PY3",
)
