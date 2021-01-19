# package(features = ["fully_static_link"])

_copts = [
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
    name = "scope",
    srcs = [
        "scope.cc",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)
cc_binary(
    name = "future",
    srcs = [
        "future.cc",
    ],
    linkopts = [
        "-l:libstdc++.a",
    ],
    features = [
        # "fully_static_link",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)

cc_binary(
    name = "singleton",
    srcs = [
        "singleton.cc",
        "singleton.h",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)

cc_binary(
    name = "test1",
    srcs = [
        "test1.cc",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)
