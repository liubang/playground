# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "libunwind",
    lib_source = ":all",
    out_static_libs = [
        "libunwind.a",
    ],
    visibility = ["//visibility:public"],
)
