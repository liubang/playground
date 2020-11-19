# vim: ft=bzl

exports_files(["LICENSE"])

cc_library(
    name = "bzip2",
    srcs = [
        "bzlib_private.h",
        "blocksort.c",
        "huffman.c",
        "crctable.c",
        "randtable.c",
        "compress.c",
        "decompress.c",
        "bzlib.c",
    ],
    hdrs = [
        "bzlib.h",
    ],
    includes = ["."],
    copts = ["-w"],
    visibility = ["//visibility:public"],
)
