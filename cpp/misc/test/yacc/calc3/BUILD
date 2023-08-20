#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/20 16:00
# Last Modified: 2023/08/20 16:00
#
#======================================================================
load("@rules_flex//flex:flex.bzl", "flex_cc_library")
load("@rules_bison//bison:bison.bzl", "bison_cc_library")

cc_library(
    name = "calc_common",
    srcs = ["calc.c"],
    hdrs = ["calc.h"],
    includes = ["."],
)

bison_cc_library(
    name = "calc_bison",
    src = "calc.y",
    deps = [":calc_common"],
)

flex_cc_library(
    name = "calc_flex",
    src = "calc.l",
    deps = [":calc_bison"],
)

cc_binary(
    name = "calc",
    srcs = [
        "main.c",
    ],
    deps = [
        ":calc_bison",
        ":calc_common",
        ":calc_flex",
    ],
)
