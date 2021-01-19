# vim: ft=bzl
load("@rules_foreign_cc//tools/build_defs:configure.bzl", "configure_make")

filegroup(
    name = "all",
    srcs = glob(["**"]),
)

configure_make(
    name = "bison_build",
    binaries = ["yacc"],
    lib_source = ":all",
    visibility = ["//visibility:public"],
)

genrule(
    name = "yacc_bin",
    srcs = [":bison_build"],
    outs = ["yacc"],
    cmd = "cp `ls $(locations :bison_build) | grep /bin/yacc$$` $@",
    executable = True,
    visibility = ["//visibility:public"],
)
