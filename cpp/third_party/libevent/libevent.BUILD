# vim: ft=bzl

load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "libevent",
    configure_options = [
        "--enable-shared=no",
    ],
    env = {
        "AR": "",
    },
    install_prefix = "lib",
    lib_source = "@libevent//:all",
    out_lib_dir = "lib",
    out_static_libs = ["libevent.a"],
    visibility = ["//visibility:public"],
)
