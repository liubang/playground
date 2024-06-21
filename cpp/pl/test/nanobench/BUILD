#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/07/30 14:36
# Last Modified: 2023/07/30 14:36
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "test",
    srcs = ["test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@nanobench//:nanobench_hdrs",
    ],
)
