#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/06/12 20:51
# Last Modified: 2023/06/12 20:51
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_binary(
    name = "test_log",
    srcs = ["test_log.cpp"],
    copts = DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/log:flags",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/log:log",
    ],
)
