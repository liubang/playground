#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/09/04 16:54
# Last Modified: 2023/09/04 16:54
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "map_benchmark",
    srcs = ["map_benchmark.cpp"],
    copts = TEST_COPTS + ["-std=c++17"],
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@google_benchmark//:benchmark_main",
    ],
)
