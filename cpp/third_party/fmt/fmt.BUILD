# vim: ft=bzl

exports_files(["LICENSE.rst"])

cc_library(
    name = "fmt",
    srcs = [
        "src/format.cc",
        "src/posix.cc",
    ],
    hdrs = [
        "include/fmt/chrono.h",
        "include/fmt/color.h",
        "include/fmt/compile.h",
        "include/fmt/core.h",
        "include/fmt/format.h",
        "include/fmt/format-inl.h",
        "include/fmt/locale.h",
        "include/fmt/ostream.h",
        "include/fmt/posix.h",
        "include/fmt/printf.h",
        "include/fmt/ranges.h",
    ],
    copts = [
        "-std=c++14",
        "-pedantic-errors",
        "-Wall",
        "-Wextra",
        "-pedantic",
        "-Wold-style-cast",
        "-Wundef",
        "-Wredundant-decls",
        "-Wwrite-strings",
        "-Wpointer-arith",
        "-Wcast-qual",
        "-Wformat=2",
        "-Wcast-align",
        "-Wnon-virtual-dtor",
        "-Wctor-dtor-privacy",
        "-Wdisabled-optimization",
        "-Winvalid-pch",
        "-Woverloaded-virtual",
        "-Wconversion",
        "-Wswitch-enum",
        "-Wno-ctor-dtor-privacy",
        "-Wno-format-nonliteral",
        "-Wno-shadow",
        "-Wnoexcept",
        "-Wno-dangling-else",
        "-Wno-unused-local-typedefs",
        "-Wdouble-promotion",
        "-Wtrampolines",
        "-Wzero-as-null-pointer-constant",
        "-Wuseless-cast",
        "-Wvector-operation-performance",
        "-Wsized-deallocation",
        "-Wshift-overflow=2",
        "-Wnull-dereference",
        "-Wduplicated-cond",
        "-Werror",
    ],
    includes = ["include"],
    visibility = ["//visibility:public"],
)