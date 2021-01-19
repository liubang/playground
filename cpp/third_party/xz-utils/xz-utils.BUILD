# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "liblzma",
    lib_source = ":all",
    static_libraries = [
        "liblzma.a",
    ],
    visibility = ["//visibility:public"],
)
