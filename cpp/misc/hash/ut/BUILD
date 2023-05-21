#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:39
# Last Modified: 2023/05/21 22:39
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "murmurhash2_test",
    srcs = [
        "murmurhash2_test.cpp",
    ],
    copts = common_copts + [
        "-std=c++20",
    ],
    linkopts = common_linkopts,
    deps = [
        "//cpp/misc/hash",
        "@googletest//:gtest_main",
    ],
)
