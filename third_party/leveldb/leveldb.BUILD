#======================================================================
#
# leveldb.BUILD -
#
# Created by liubang on 2023/08/17 23:23
# Last Modified: 2023/08/17 23:23
#
#======================================================================

load("@bazel_skylib//rules:copy_file.bzl", "copy_file")

copy_file(
    name = "port_config_h",
    src = "@//third_party/leveldb:port_config.h",
    out = "port/port_config.h",
    allow_symlink = True,
)

copy_file(
    name = "port_h",
    src = "@//third_party/leveldb:port.h",
    out = "port/port.h",
    allow_symlink = True,
)

cc_library(
    name = "leveldb",
    srcs = glob(
        [
            "db/**/*.cc",
            "db/**/*.h",
            "helpers/**/*.cc",
            "helpers/**/*.h",
            "port/**/*.cc",
            "port/**/*.h",
            "table/**/*.cc",
            "table/**/*.h",
            "util/**/*.cc",
            "util/**/*.h",
        ],
        exclude = [
            "**/*_test.cc",
            "**/testutil.*",
            "**/*_bench.cc",
            "**/*_windows*",
            "db/leveldbutil.cc",
        ],
    ),
    hdrs = glob(
        ["include/**/*.h"],
        exclude = ["doc/**"],
    ) + [
        ":port_config_h",
        ":port_h",
    ],
    copts = ["-std=c++17"],
    includes = [
        ".",
        "include",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@crc32c",
        "@snappy",
    ],
)
