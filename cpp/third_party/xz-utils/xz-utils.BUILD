# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "liblzma",
    lib_source = ":all",
    out_static_libs = [
        "liblzma.a",
    ],
    visibility = ["//visibility:public"],
)
