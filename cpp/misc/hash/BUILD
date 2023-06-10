#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 22:39
# Last Modified: 2023/05/21 22:39
#
#======================================================================
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "hash",
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
)

# cc_test(
#     name = "murmurhash2_test",
#     srcs = [
#         "murmurhash2_test.cpp",
#     ],
#     copts = [
#         "-std=c++20",
#     ] + TEST_COPTS,
#     linkopts = DEFAULT_LINKOPTS,
#     deps = [
#         "//cpp/misc/hash",
#         "@googletest//:gtest_main",
#     ],
# )
