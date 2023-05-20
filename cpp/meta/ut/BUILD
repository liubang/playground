#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:59
# Last Modified: 2023/05/21 00:59
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "expression_test",
    srcs = [
        "expression_test.cpp",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "traits_test",
    srcs = [
        "traits_test.cpp",
        "traits_test.h",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "utils_test",
    srcs = [
        "utils_test.cpp",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)
