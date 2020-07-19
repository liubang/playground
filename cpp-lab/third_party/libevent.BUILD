# vim: ft=bzl

load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "libevent",
    configure_options = [
        "--enable-shared=no",
    ],
    install_prefix = "lib",
    lib_source = "@libevent//:all",
    out_lib_dir = "lib",
    static_libraries = ["libevent.a"],
    configure_env_vars = {
        "AR": "",
    },
    visibility = ["//visibility:public"],
)
