#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/20 01:30
# Last Modified: 2023/08/20 01:30
#
#======================================================================
load("@rules_flex//flex:flex.bzl", "flex_cc_library")
load("@rules_bison//bison:bison.bzl", "bison_cc_library")

cc_library(
    name = "calc_common",
    srcs = ["calc2.c"],
    hdrs = ["calc2.h"],
    includes = ["."],
)

bison_cc_library(
    name = "calc2_bison",
    src = "calc2.y",
    deps = [":calc_common"],
)

flex_cc_library(
    name = "calc2_flex",
    src = "calc2.l",
    deps = [":calc2_bison"],
)

cc_binary(
    name = "calc2",
    srcs = [
        "main.c",
    ],
    deps = [
        ":calc2_bison",
        ":calc2_flex",
        ":calc_common",
    ],
)
