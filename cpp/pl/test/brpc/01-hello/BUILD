#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/17 23:54
# Last Modified: 2023/08/17 23:54
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_binary(
    name = "test",
    srcs = ["test.cpp"],
    copts = ["-std=c++17"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@brpc",
        "@brpc//:bthread",
        "@brpc//:butil",
        "@brpc//:bvar",
    ],
)
