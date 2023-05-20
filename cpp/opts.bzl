#======================================================================
#
# opts.bzl -
#
# Created by liubang on 2023/05/21 01:23
# Last Modified: 2023/05/21 01:23
#
#======================================================================

common_copts = [
    "-fno-omit-frame-pointer",
    "-fsanitize=address",
]

common_linkopts = [
    "-fsanitize=address",
]
