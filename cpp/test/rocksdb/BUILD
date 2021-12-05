_DEFAULT_COPTS = [
    "-std=gnu++17",
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wno-unused-local-typedefs",
    "-Wno-unused-parameter",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-msse3",
    "-msse4",
    "-msse4.1",
    "-mpclmul",
    "-fno-builtin-memcmp",
]

cc_binary(
    name = "basic_operation",
    srcs = [
        "basic_operation.cc",
    ],
    copts = _DEFAULT_COPTS,
    features = [
        "fully_static_link",
    ],
    linkopts = ["-l:libstdc++.a"],
    deps = [
        "@folly",
        "@rocksdb",
    ],
)

cc_binary(
    name = "use_memtable",
    srcs = [
        "use_memtable.cc",
    ],
    copts = _DEFAULT_COPTS,
    features = [
        "fully_static_link",
    ],
    linkopts = ["-l:libstdc++.a"],
    deps = [
        "@folly",
        "@rocksdb",
    ],
)
