#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 23:30
# Last Modified: 2023/05/21 23:30
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_binary(
    name = "test_async",
    srcs = ["test_async.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts + [
        "-pthread",
    ],
)

cc_binary(
    name = "test_context",
    srcs = ["test_context.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
)
