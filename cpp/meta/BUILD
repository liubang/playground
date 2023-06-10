#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 01:00
# Last Modified: 2023/05/21 01:00
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_library(
    name = "meta",
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)
