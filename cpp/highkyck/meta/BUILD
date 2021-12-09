_COPTS = [
    "-std=gnu++17",
]

cc_binary(
    name = "factorial",
    srcs = [
        "factorial.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "forward",
    srcs = [
        "forward.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "forward2",
    srcs = [
        "forward2.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "deducing_type_test",
    srcs = [
        "deducing_type.h",
        "deducing_type_test.cpp",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "is_nothrow_xxxx_constructible",
    srcs = [
        "is_nothrow_xxxx_constructible.cc",
    ],
    copts = _COPTS,
)

cc_library(
    name = "common",
    hdrs = [
        "common.h",
    ],
    copts = _COPTS,
)

cc_library(
    name = "scope_guard",
    hdrs = ["scope_guard.h"],
    deps = [
        ":common",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "scope_guard_test",
    srcs = [
        "scope_guard_test.cpp",
    ],
    copts = _COPTS,
    deps = [
        ":scope_guard",
    ],
)

cc_binary(
    name = "common_type",
    srcs = [
        "common_type.cpp",
        "common.h",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "enable_if_test",
    srcs = [
        "enable_if.h",
        "enable_if_test.cpp",
    ],
    copts = _COPTS,
)
