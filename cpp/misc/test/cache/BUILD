#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/10/15 23:34
# Last Modified: 2023/10/15 23:34
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = [
            "-std=c++20",
            "-ffast-math",
            "-march=native",
        ] + TEST_COPTS + select({
            "//:linux_x86_64": ["-fopenmp"],
            "//conditions:default": [],
        }),
        linkopts = DEFAULT_LINKOPTS + select({
            "//:linux_x86_64": ["-fopenmp"],
            "//conditions:default": [],
        }),
        deps = [
            "//cpp/tools",
            "@google_benchmark//:benchmark_main",
        ],
    )
    for f in glob(["*.cpp"])
]
