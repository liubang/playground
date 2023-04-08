# vim: ft=bzl

load("@rules_foreign_cc//foreign_cc:defs.bzl", "make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

make(
    name = "libaio",
    lib_source = ":all",
    out_static_libs = [
        "libaio.a",
    ],
    visibility = ["//visibility:public"],
)
