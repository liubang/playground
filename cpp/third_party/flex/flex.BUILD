# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "flex_build",
    lib_source = ":all",
    out_binaries = ["flex"],
    visibility = ["//visibility:public"],
)

genrule(
    name = "flex_bin",
    srcs = [":flex_build"],
    outs = [":flex"],
    cmd = "cp `ls $(locations :flex_build) | grep /bin/flex$$` $@",
    executable = True,
    visibility = ["//visibility:public"],
)
