# vim: ft=bzl

exports_files(["LICENSE"])

genrule(
    name = "folly_config",
    outs = ["folly/folly-config.h"],
    cmd = "\n".join([
        "cat << 'EOF' >$@",
        "#pragma once",
        "#if !defined(FOLLY_MOBILE)",
        "#if defined(__ANDROID__) || \\",
        "    (defined(__APPLE__) &&  \\",
        "     (TARGET_IPHONE_SIMULATOR || TARGET_OS_SIMULATOR || TARGET_OS_IPHONE))",
        "#define FOLLY_MOBILE 1",
        "#else",
        "#define FOLLY_MOBILE 0",
        "#endif",
        "#endif // FOLLY_MOBILE",
        "#define FOLLY_HAVE_PTHREAD 1",
        "#define FOLLY_HAVE_PTHREAD_ATFORK 1",
        "#define FOLLY_HAVE_LIBGFLAGS 1",
        "#define FOLLY_UNUSUAL_GFLAGS_NAMESPACE 0",
        "#define FOLLY_GFLAGS_NAMESPACE google",
        "#define FOLLY_HAVE_LIBGLOG 1",
        "#define FOLLY_HAVE_MALLOC_H 1",
        "#define FOLLY_HAVE_BITS_CXXCONFIG_H 1",
        "#define FOLLY_HAVE_FEATURES_H 1",
        "#if FOLLY_HAVE_FEATURES_H",
        "#include <features.h>",
        "#endif",
        "#define FOLLY_HAVE_MEMRCHR 1",
        "#define FOLLY_HAVE_PREADV 1",
        "#define FOLLY_HAVE_PWRITEV 1",
        "#define FOLLY_HAVE_CLOCK_GETTIME 1",
        "#define FOLLY_HAVE_OPENSSL_ASN1_TIME_DIFF 1",
        "#define FOLLY_HAVE_IFUNC 1",
        "#define FOLLY_HAVE_STD__IS_TRIVIALLY_COPYABLE 1",
        "#define FOLLY_HAVE_UNALIGNED_ACCESS 1",
        "#define FOLLY_HAVE_VLA 1",
        "#define FOLLY_HAVE_WEAK_SYMBOLS 1",
        "#define FOLLY_HAVE_LINUX_VDSO 1",
        "#define FOLLY_HAVE_MALLOC_USABLE_SIZE 1",
        "#define FOLLY_HAVE_INT128_T 1",
        "#define FOLLY_HAVE_WCHAR_SUPPORT 1",
        "#define FOLLY_HAVE_EXTRANDOM_SFMT19937 1",
        "#define HAVE_VSNPRINTF_ERRORS 1",
        "#define FOLLY_USE_SYMBOLIZER 1",
        "#define FOLLY_DEMANGLE_MAX_SYMBOL_SIZE 1024",
        "#define FOLLY_HAVE_SHADOW_LOCAL_WARNINGS 1",
        "#define FOLLY_HAVE_LIBLZ4 1",
        "#define FOLLY_HAVE_LIBSNAPPY 1",
        "#define FOLLY_HAVE_LIBZ 1",
        "#define FOLLY_HAVE_LIBZSTD 1",
        "#define FOLLY_HAVE_LIBBZ2 1",
        "EOF",
    ]),
)

_common_hdrs = glob(["folly/**/*.h"]) + [
    ":folly_config",
]

_common_copts = [
    "-std=gnu++1z",
    "-Wall",
    "-Wextra",
    "-fsigned-char",
    "-Wno-deprecated",
    "-Wno-deprecated-declarations",
    "-Wno-sign-compare",
    "-Wno-unused",
    "-Wunused-label",
    "-Wunused-result",
    "-Wshadow-compatible-local",
    "-Wno-noexcept-type",
    "-fopenmp",
    "-faligned-new",
    "-fno-builtin-memcmp",
    "-finput-charset=UTF-8",
    "-fno-omit-frame-pointer",
    "-momit-leaf-frame-pointer",
]

pclmul_files = [
    "folly/hash/detail/ChecksumDetail.cpp",
    "folly/hash/detail/Crc32CombineDetail.cpp",
    "folly/hash/detail/Crc32cDetail.cpp",
]

cc_library(
    name = "lib_pclmul_files",
    srcs = pclmul_files,
    hdrs = _common_hdrs,
    includes = [".", "folly"],
    copts = _common_copts + [
        "-mpclmul",
    ],
    deps = [
        "@boost//:program_options",
    ],
)

cc_library(
    name = "MathOperation_AVX2",
    srcs = [
        "folly/experimental/crypto/detail/MathOperation_AVX2.cpp",
    ],
    hdrs = _common_hdrs,
    includes = [".", "folly"],
    linkstatic = True,
    copts = _common_copts + [
        "-mavx",
        "-mavx2",
        "-msse2",
    ],
    deps = [
        "@glog",
        "@boost//:program_options",
        "@libsodium//:libsodium",
    ],
)

cc_library(
    name = "MathOperation_Simple",
    srcs = [
        "folly/experimental/crypto/detail/MathOperation_Simple.cpp",
    ],
    hdrs = _common_hdrs,
    includes = [".", "folly"],
    linkstatic = True,
    copts = _common_copts + [
        "-mno-avx",
        "-mno-avx2",
        "-mno-sse2",
    ],
    deps = [
        "@glog",
        "@boost//:program_options",
    ],
)

cc_library(
    name = "MathOperation_SSE2",
    srcs = [
        "folly/experimental/crypto/detail/MathOperation_SSE2.cpp",
    ],
    hdrs = _common_hdrs,
    includes = [".", "folly"],
    linkstatic = True,
    copts = _common_copts + [
        "-mno-avx",
        "-mno-avx2",
        "-msse2",
    ],
    deps = [
        "@glog",
        "@boost//:program_options",
        "@libsodium//:libsodium",
    ],
)

cc_library(
    name = "folly",
    includes = [".", "folly"],
    srcs = glob(
        ["folly/**/*.cpp"],
        exclude = [
            "folly/build/**/*.cpp",
            "folly/experimental/exception_tracer/**/*.cpp",
            "folly/experimental/pushmi/**/*.cpp",
            "folly/futures/exercises/**/*.cpp",
            "folly/logging/example/**/*.cpp",
            "folly/**/test/**/*.cpp",
            "folly/tools/**/*.cpp",
            "folly/**/*Benchmark.cpp",
            "folly/**/*Test.cpp",
            "folly/experimental/JSONSchemaTester.cpp",
            "folly/experimental/io/HugePageUtil.cpp",
            "folly/python/fibers.cpp",
            "folly/python/GILAwareManualExecutor.cpp",
            "folly/cybld/folly/executor.cpp",
            "folly/experimental/crypto/detail/MathOperation_AVX2.cpp",
            "folly/experimental/crypto/detail/MathOperation_Simple.cpp",
            "folly/experimental/crypto/detail/MathOperation_SSE2.cpp",
        ] + pclmul_files,
    ) + [
        "folly/io/async/test/ScopedBoundPort.cpp",
        "folly/io/async/test/SocketPair.cpp",
        "folly/io/async/test/TimeUtil.cpp",
        ":folly_config",
    ],
    hdrs = _common_hdrs,
    copts = _common_copts,
    linkopts = [
        "-ldl",
        "-pthread",
        "-lunwind",
    ],
    linkstatic = True,
    deps = [
        ":lib_pclmul_files",
        ":MathOperation_AVX2",
        ":MathOperation_Simple",
        ":MathOperation_SSE2",
        "@boost//:context",
        "@boost//:filesystem",
        "@boost//:program_options",
        "@boost//:regex",
        "@boost//:system",
        "@boost//:thread",
        "@openssl//:openssl",
        "@libaio//:libaio",
        "@snappy//:snappy",
        "@libevent//:libevent",
        "@lz4//:lz4_frame",
        "@zlib//:zlib",
        "@dwarf//:dwarf",
        "@zstd//:zstd",
        "@liburing//:liburing",
        "@bzip2//:bzip2",
        "@libsodium//:libsodium",
        "@fmt//:fmt",
        "@glog//:glog",
        "@double-conversion//:double-conversion",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "folly_exception_tracer_base",
    srcs = [
        "folly/experimental/exception_tracer/ExceptionTracer.cpp",
        "folly/experimental/exception_tracer/StackTrace.cpp",
    ],
    copts = _common_copts,
    linkstatic = True,
    deps = [
        ":folly",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "folly_exception_tracer",
    srcs = [
        "folly/experimental/exception_tracer/ExceptionStackTraceLib.cpp",
        "folly/experimental/exception_tracer/ExceptionTracerLib.cpp",
    ],
    copts = _common_copts,
    linkstatic = True,
    deps = [
        ":folly_exception_tracer_base",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "folly_exception_counter",
    srcs = [
        "folly/experimental/exception_tracer/ExceptionCounterLib.cpp",
    ],
    copts = _common_copts,
    linkstatic = True,
    deps = [
        ":folly_exception_tracer",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "follybenchmark",
    srcs = [
        "folly/Benchmark.cpp",
    ],
    copts = _common_copts,
    linkstatic = True,
    deps = [
        ":folly",
    ],
    visibility = ["//visibility:public"],
)
