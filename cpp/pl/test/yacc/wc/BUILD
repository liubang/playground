#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/08/20 01:19
# Last Modified: 2023/08/20 01:19
#
#======================================================================

load("@rules_flex//flex:flex.bzl", "flex")

flex(
    name = "wc_flex",
    src = "wc.l",
)

cc_binary(
    name = "wc",
    srcs = [":wc_flex"],
)
