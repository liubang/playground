# vim: ft=bzl

licenses(["restricted"])  # GPLv2

exports_files([
    "LICENSE",
    "COPYING",
])

cc_library(
    name = "zstd",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        ":compress",
        ":decompress",
        ":deprecated",
        ":zdict",
    ],
)

cc_library(
    name = "common",
    srcs = glob(["lib/common/*.c"]),
    hdrs = [
        "lib/common/bitstream.h",
        "lib/common/compiler.h",
        "lib/common/cpu.h",
        "lib/common/debug.h",
        "lib/common/error_private.h",
        "lib/common/fse.h",
        "lib/common/huf.h",
        "lib/common/mem.h",
        "lib/common/pool.h",
        "lib/common/threading.h",
        "lib/common/xxhash.h",
        "lib/common/zstd_errors.h",
        "lib/common/zstd_internal.h",
        "lib/zstd.h",
    ],
    copts = ["-w"],
    includes = [
        "lib",
        "lib/common",
    ],
)

cc_library(
    name = "zdict",
    srcs = glob(["lib/dictBuilder/*.c"]),
    hdrs = glob(["lib/dictBuilder/*.h"]),
    copts = ["-w"],
    includes = ["lib/dictBuilder"],
    deps = [":common"],
)

cc_library(
    name = "compress",
    srcs = glob(["lib/compress/*.c"]),
    hdrs = glob(["lib/compress/*.h"]),
    copts = ["-w"],
    includes = ["lib/common"],
    deps = [":common"],
)

cc_library(
    name = "legacy",
    srcs = glob(["lib/legacy/*.c"]),
    hdrs = glob(["lib/legacy/*.h"]),
    copts = ["-w"],
    includes = ["lib/common"],
    local_defines = [
        "ZSTD_LEGACY_SUPPORT=4",
    ],
    deps = [":common"],
)

cc_library(
    name = "decompress",
    srcs = glob(["lib/decompress/*.c"]),
    hdrs = glob(["lib/decompress/*.h"]),
    copts = ["-w"],
    includes = [
        "lib/common",
    ],
    deps = [
        ":common",
        ":legacy",
    ],
)

cc_library(
    name = "deprecated",
    srcs = glob(["lib/deprecated/*.c"]),
    hdrs = glob(["lib/deprecated/*.h"]),
    copts = ["-w"],
    includes = ["lib/common"],
    deps = [":common"],
)
