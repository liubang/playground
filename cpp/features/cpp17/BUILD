#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/10/04 19:19
# Last Modified: 2023/10/04 19:19
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
            "-std=c++17",
        ] + TEST_COPTS,
        linkopts = DEFAULT_LINKOPTS,
        deps = [
            "//cpp/tools"
        ]
    )

    for f in glob(["*.cpp"])
]
