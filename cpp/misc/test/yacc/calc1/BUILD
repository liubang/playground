#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/19 16:01
# Last Modified: 2023/08/19 16:01
#
#======================================================================

load("@rules_flex//flex:flex.bzl", "flex")
load("@rules_bison//bison:bison.bzl", "bison")

flex(
    name = "calc_flex",
    src = "calc.l",
)

bison(
    name = "calc_bison",
    src = "calc.y",
)

cc_library(
    name = "calc_common",
    srcs = [
        "calc.c",
        ":calc_bison",
        ":calc_flex",
    ],
    hdrs = ["calc.h"],
    includes = ["."],
)

cc_binary(
    name = "calc",
    srcs = ["main.c"],
    copts = ["-std=c99"],
    deps = [
        ":calc_common",
    ],
)
