#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/09/17 23:04
# Last Modified: 2023/09/17 23:04
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_library(
    name = "kaleidoscope_lib",
    srcs = [
        "ast.cpp",
        "laxer.cpp",
    ],
    hdrs = [
        "ast.h",
        "laxer.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "kaleidoscope",
    srcs = ["main.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":kaleidoscope_lib",
    ],
)
