# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "jemalloc",
    configure_command = "configure",
    install_prefix = "lib",
    configure_options = [],
    static_libraries = [
        "libjemalloc.a",
    ],
    lib_source = ":all",
    out_lib_dir = "lib",
    out_include_dir = "include",
    visibility = ["//visibility:public"],
)

