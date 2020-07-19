load("//:copts/configure_copts.bzl", "DEFAULT_COPTS")

cc_binary(
    name = "basic_operation",
    srcs = [
        "basic_operation.cc",
    ],
    copts = DEFAULT_COPTS,
    deps = [
        "@rocksdb",
        "@folly",
    ],
)

cc_binary(
    name = "use_memtable",
    srcs = [
        "use_memtable.cc",
    ],
    copts = DEFAULT_COPTS,
    deps = [
        "@rocksdb",
        "@folly",
    ],
)
