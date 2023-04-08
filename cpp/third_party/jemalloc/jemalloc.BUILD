# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "jemalloc",
    configure_command = "configure",
    install_prefix = "lib",
    lib_source = ":all",
    out_include_dir = "include",
    out_lib_dir = "lib",
    out_static_libs = [
        "libjemalloc.a",
    ],
    visibility = ["//visibility:public"],
)
