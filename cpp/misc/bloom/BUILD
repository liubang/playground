#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:38
# Last Modified: 2023/05/21 22:38
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "bloom_filter",
    srcs = ["bloom_filter.cpp"],
    hdrs = ["bloom_filter.h"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/misc/hash",
        "//cpp/tools",
    ],
)

# cc_test(
#     name = "bloom_filter_test",
#     srcs = [
#         "bloom_filter_test.cpp",
#     ],
#     copts = [
#         "-std=c++20",
#     ] + TEST_COPTS,
#     linkopts = DEFAULT_LINKOPTS,
#     deps = [
#         "//cpp/misc/bloom:bloom_filter",
#         "@googletest//:gtest_main",
#     ],
# )
