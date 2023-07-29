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
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "common",
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
        ] + TEST_COPTS + select({
            "//:linux_x86_64": ["-fopenmp"],
            "//conditions:default": [],
        }),
        linkopts = DEFAULT_LINKOPTS + select({
            "//:linux_x86_64": ["-fopenmp"],
            "//conditions:default": [],
        }),
        deps = [
            ":common",
            "@nanobench",
        ],
    )
    for f in glob(["*.cpp"])
]
