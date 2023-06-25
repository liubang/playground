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
            "@com_github_google_benchmark//:benchmark_main",
            "@oneTBB//:tbb",
        ],
    )
    for f in glob(["*_benchmark.cpp"])
]

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
            "//cpp/tools",
            "@oneTBB//:tbb",
        ],
    )
    for f in glob(["test_*.cpp"])
]
