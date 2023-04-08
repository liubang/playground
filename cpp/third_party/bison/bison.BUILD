# vim: ft=bzl
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "bison_build",
    lib_source = ":all",
    out_binaries = [
        "yacc",
        "bison",
    ],
    visibility = ["//visibility:public"],
)

genrule(
    name = "yacc_bin",
    srcs = [":bison_build"],
    outs = ["yacc"],
    cmd = "cp `ls $(locations :bison_build) | grep /bin/bison$$` $@",
    executable = True,
    visibility = ["//visibility:public"],
)
