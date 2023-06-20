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
            "-fopenmp",
        ] + TEST_COPTS,
        linkopts = [
            "-fopenmp",
        ] + DEFAULT_LINKOPTS,
        deps = [
            ":common",
            "@com_github_google_benchmark//:benchmark_main",
        ],
    )
    for f in glob(["*.cpp"])
]
