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

bison(
    name = "calc_bison",
    src = "calc.y",
)

flex(
    name = "calc_flex",
    src = "calc.l",
)

cc_binary(
    name = "calc",
    srcs = [
        ":calc_bison",
        ":calc_flex",
    ],
    copts = ["-std=c99"],
)
