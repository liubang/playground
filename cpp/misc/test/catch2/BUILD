#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/07/29 22:26
# Last Modified: 2023/07/29 22:26
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "test_fibonacci",
    srcs = ["fibonacci.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@catch2//:catch2_main",
    ],
)
