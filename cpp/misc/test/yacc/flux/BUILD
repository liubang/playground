#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/10/30 20:01
# Last Modified: 2023/10/30 20:01
#
#======================================================================
load("@rules_flex//flex:flex.bzl", "flex", "flex_cc_library")
load("@rules_bison//bison:bison.bzl", "bison", "bison_cc_library")

flex(
    name = "flux_lexer",
    src = "flux_lexer.l",
)

bison(
    name = "flux_parser",
    src = "flux_parser.y",
)

cc_library(
    name = "flux_common",
    srcs = [
        "flux.c",
        ":flux_lexer",
        ":flux_parser",
    ],
    hdrs = ["flux.h"],
    includes = ["."],
)

cc_binary(
    name = "flux",
    srcs = ["main.c"],
    copts = ["-std=c11"],
    deps = [
        ":calc_common",
    ],
)
