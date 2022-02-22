# vim: ft=bzl

package(default_visibility = ["//visibility:public"])

_common_copts = [
    "-fno-omit-frame-pointer",
    "-fsanitize=address",
]

_common_linkopts = [
    "-fsanitize=address",
]

cc_test(
    name = "test_tuple_iteration_basics",
    srcs = [
        "test_tuple_iteration_basics.cpp",
        "tuple_iteration_basics.h",
    ],
    copts = _common_copts + [
        "-std=c++17",
    ],
    linkopts = _common_linkopts,
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "test_tuple_iteration_apply",
    srcs = [
        "test_tuple_iteration_apply.cpp",
        "tuple_iteration_apply.h",
    ],
    copts = _common_copts + [
        "-std=c++20",
    ],
    linkopts = _common_linkopts,
    deps = [
        "@gtest//:gtest_main",
    ],
)
