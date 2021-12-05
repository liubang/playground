# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "jemalloc",
    configure_command = "configure",
    configure_options = [],
    install_prefix = "lib",
    lib_source = ":all",
    out_include_dir = "include",
    out_lib_dir = "lib",
    static_libraries = [
        "libjemalloc.a",
    ],
    visibility = ["//visibility:public"],
)
