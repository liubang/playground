#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/19 23:36
# Last Modified: 2023/06/19 23:36
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_library(
    name = "common",
    hdrs = glob(["*.h"]),
)

[
    cc_binary(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
            "-fopenmp",
        ] + DEFAULT_COPTS,
        linkopts = [
            "-fopenmp",
        ] + DEFAULT_LINKOPTS,
        deps = [
            ":common",
            "//cpp/tools",
        ],
    )
    for f in glob(["*.cpp"])
]
