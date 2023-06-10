#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:32
# Last Modified: 2023/05/21 00:32
#
#======================================================================

load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_library(
    name = "tools",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = ["-pthread"] + DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "@boost//:crc",
    ],
)
