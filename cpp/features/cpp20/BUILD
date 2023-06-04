#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/04 16:30
# Last Modified: 2023/06/04 16:30
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

[
    cc_test(
        name = "%s" % f[:f.rfind(".")],
        srcs = [
            "%s" % f,
        ],
        copts = common_copts + [
            "-std=c++20",
        ],
        linkopts = common_linkopts,
        deps = [
            "@googletest//:gtest_main",
        ],
    )
    for f in glob(["*.cpp"])
]
