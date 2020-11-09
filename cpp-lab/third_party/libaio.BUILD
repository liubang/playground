# vim: ft=bzl

cc_library(
    name = "libaio",
    srcs = glob(["src/*.c"]),
    hdrs = glob(["src/*.h"]),
    includes = ["src"],
    copts = [
        "-fomit-frame-pointer",
        "-O2",
        "-nostdlib",
        "-nostartfiles",
        "-Wall",
    ],
    visibility = ["//visibility:public"],
)
