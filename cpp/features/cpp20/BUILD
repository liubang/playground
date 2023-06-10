#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/04 16:30
# Last Modified: 2023/06/04 16:30
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
        ] + TEST_COPTS,
        linkopts = DEFAULT_LINKOPTS,
        deps = [
            "@googletest//:gtest_main",
        ],
    )
    for f in glob(["*.cpp"])
]
