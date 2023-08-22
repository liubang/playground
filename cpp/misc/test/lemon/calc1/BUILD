#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/22 00:37
# Last Modified: 2023/08/22 00:37
#
#======================================================================

genrule(
    name = "calc_lemon",
    srcs = [
        "calc.y",
        "ttokens.h",
        "@sqlite//:lempar",
    ],
    outs = [
        "calc.c",
    ],
    cmd = "$(location @sqlite//:lemon) -T$(location @sqlite//:lempar) -d$(RULEDIR) -s $(location calc.y)",
    tools = [
        "@sqlite//:lemon",
    ],
)

cc_binary(
    name = "calc",
    srcs = [
        "main.c",
        "ttokens.h",
        "types.h",
        ":calc_lemon",
    ],
    includes = ["."],
)
