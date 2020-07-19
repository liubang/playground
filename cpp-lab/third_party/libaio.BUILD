# vim: ft=bzl

cc_library(
    name = "libaio",
    srcs = glob(["src/*.c"]),
    hdrs = glob(["src/*.h"]),
    includes = ["src"],
    linkstatic = True,
    copts = [
        "-fomit-frame-pointer",
        "-O2",
        "-nostdlib",
        "-nostartfiles",
        "-Wall",
    ],
    visibility = ["//visibility:public"],
)
