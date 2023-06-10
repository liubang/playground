#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 23:03
# Last Modified: 2023/05/21 23:03
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "yabf_test",
    srcs = ["yabf_test.cpp"],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/misc/bf:yabf",
        "@googletest//:gtest_main",
    ],
)
