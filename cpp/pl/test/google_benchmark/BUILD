#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/14 00:25
# Last Modified: 2023/08/14 00:25
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "fibonacci",
    srcs = ["fibonacci.cpp"],
    copts = TEST_COPTS + ["-std=c++17"],
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@google_benchmark//:benchmark_main",
    ],
)
