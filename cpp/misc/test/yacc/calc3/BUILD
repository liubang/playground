#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/20 16:00
# Last Modified: 2023/08/20 16:00
#
#======================================================================
load("@rules_flex//flex:flex.bzl", "flex", "flex_cc_library")
load("@rules_bison//bison:bison.bzl", "bison", "bison_cc_library")

bison(
    name = "calc_bison",
    src = "calc.y",
)

flex(
    name = "calc_flex",
    src = "calc.l",
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
    copts = ["-std=c11"],
    deps = [
        ":calc_common",
    ],
)
