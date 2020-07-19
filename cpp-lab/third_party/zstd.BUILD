# vim: ft=bzl

licenses(["restricted"])  # GPLv2

exports_files(["LICENSE", "COPYING"])

cc_library(
    name = "zstd",
    deps = [
        ":common",
        ":zdict",
        ":compress",
        ":decompress",
        ":deprecated",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "common",
    includes = ["lib", "lib/common"],
    srcs = glob(["lib/common/*.c"]),
    hdrs = [
        "lib/common/debug.h",
        "lib/common/xxhash.h",
        "lib/common/threading.h",
        "lib/common/cpu.h",
        "lib/common/fse.h",
        "lib/common/zstd_errors.h",
        "lib/common/compiler.h",
        "lib/common/pool.h",
        "lib/common/huf.h",
        "lib/common/mem.h",
        "lib/common/error_private.h",
        "lib/common/zstd_internal.h",
        "lib/common/bitstream.h",
        "lib/zstd.h",
    ],
    copts = ["-w"],
    linkstatic = True,
)

cc_library(
    name = "zdict",
    srcs = glob(["lib/dictBuilder/*.c"]),
    hdrs = glob(["lib/dictBuilder/*.h"]),
    includes = ["lib/dictBuilder"],
    deps = [":common"],
    copts = ["-w"],
    linkstatic = True,
)

cc_library(
    name = "compress",
    includes = ["lib/common"],
    srcs = glob(["lib/compress/*.c"]),
    hdrs = glob(["lib/compress/*.h"]),
    copts = ["-w"],
    deps = [":common"],
    linkstatic = True,
)

cc_library(
    name = "legacy",
    includes = ["lib/common"],
    srcs = glob(["lib/legacy/*.c"]),
    hdrs = glob(["lib/legacy/*.h"]),
    local_defines = [
        "ZSTD_LEGACY_SUPPORT=4",
    ],
    copts = ["-w"],
    deps = [":common"],
    linkstatic = True,
)

cc_library(
    name = "decompress",
    includes = [
        "lib/common",
    ],
    srcs = glob(["lib/decompress/*.c"]),
    hdrs = glob(["lib/decompress/*.h"]),
    copts = ["-w"],
    deps = [":common", ":legacy"],
    linkstatic = True,
)

cc_library(
    name = "deprecated",
    includes = ["lib/common"],
    srcs = glob(["lib/deprecated/*.c"]),
    hdrs = glob(["lib/deprecated/*.h"]),
    copts = ["-w"],
    deps = [":common"],
    linkstatic = True,
)
