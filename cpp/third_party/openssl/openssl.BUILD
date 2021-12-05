# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "openssl",
    configure_command = "config",
    configure_options = [
        "no-shared",
    ],
    lib_source = ":all",
    static_libraries = [
        "libssl.a",
        "libcrypto.a",
    ],
    visibility = ["//visibility:public"],
)
