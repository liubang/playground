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
    static_libraries = [
        "libssl.a",
        "libcrypto.a",
    ],
    lib_source = ":all",
    visibility = ["//visibility:public"],
)
