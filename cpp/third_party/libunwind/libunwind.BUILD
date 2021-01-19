# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "libunwind",
    lib_source = ":all",
    static_libraries = [
        "libunwind.a",
    ],
    visibility = ["//visibility:public"],
)
