#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 23:03
# Last Modified: 2023/05/21 23:03
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "yabf_test",
    srcs = ["yabf_test.cpp"],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/bf:yabf",
        "@googletest//:gtest_main",
    ],
)
