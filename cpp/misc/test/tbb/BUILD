#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/17 22:26
# Last Modified: 2023/06/17 22:26
#
#======================================================================

load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_library(
    name = "tbb_common",
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
        ] + DEFAULT_COPTS,
        linkopts = DEFAULT_LINKOPTS,
        deps = [
            ":tbb_common",
            "@oneTBB//:tbb",
        ],
    )
    for f in glob(["*.cpp"])
]
