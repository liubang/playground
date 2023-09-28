#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/09/28 16:54
# Last Modified: 2023/09/28 16:54
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "fsha",
    srcs = [],
    hdrs = [
        "fs_adaptor.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)
