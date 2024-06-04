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
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "tbb_common",
    hdrs = glob(["*.h"]),
)

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
        ] + TEST_COPTS,
        linkopts = DEFAULT_LINKOPTS,
        deps = [
            ":tbb_common",
            "@nanobench",
            "@onetbb//:tbb",
        ],
    )
    for f in glob(["*.cpp"])
]
