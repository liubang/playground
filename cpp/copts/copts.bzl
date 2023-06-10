#======================================================================
#
# copts.bzl -
#
# Created by liubang on 2023/06/11 00:19
# Last Modified: 2023/06/11 00:19
#
#======================================================================

GCC_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Wformat-security",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wundef",
    "-Wunused-local-typedefs",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-fsanitize=address",
    "-DNOMINMAX",
]

GCC_TEST_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Wformat-security",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wundef",
    "-Wunused-local-typedefs",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-DNOMINMAX",
    "-Wno-deprecated-declarations",
    "-Wno-missing-declarations",
    "-Wno-self-move",
    "-Wno-sign-compare",
    "-Wno-unused-function",
    "-Wno-unused-parameter",
    "-Wno-unused-private-field",
]

LLVM_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion",
    "-Wfloat-overflow-conversion",
    "-Wfloat-zero-conversion",
    "-Wfor-loop-analysis",
    "-Wformat-security",
    "-Wgnu-redeclared-enum",
    "-Winfinite-recursion",
    "-Winvalid-constexpr",
    "-Wliteral-conversion",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wself-assign",
    "-Wshadow-all",
    "-Wshorten-64-to-32",
    "-Wsign-conversion",
    "-Wstring-conversion",
    "-Wtautological-overlap-compare",
    "-Wtautological-unsigned-zero-compare",
    "-Wundef",
    "-Wuninitialized",
    "-Wunreachable-code",
    "-Wunused-comparison",
    "-Wunused-local-typedefs",
    "-Wunused-result",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-float-conversion",
    "-Wno-implicit-float-conversion",
    "-Wno-implicit-int-float-conversion",
    "-Wno-unknown-warning-option",
    "-DNOMINMAX",
]

LLVM_TEST_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion",
    "-Wfloat-overflow-conversion",
    "-Wfloat-zero-conversion",
    "-Wfor-loop-analysis",
    "-Wformat-security",
    "-Wgnu-redeclared-enum",
    "-Winfinite-recursion",
    "-Winvalid-constexpr",
    "-Wliteral-conversion",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wself-assign",
    "-Wshadow-all",
    "-Wstring-conversion",
    "-Wtautological-overlap-compare",
    "-Wtautological-unsigned-zero-compare",
    "-Wundef",
    "-Wuninitialized",
    "-Wunreachable-code",
    "-Wunused-comparison",
    "-Wunused-local-typedefs",
    "-Wunused-result",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-float-conversion",
    "-Wno-implicit-float-conversion",
    "-Wno-implicit-int-float-conversion",
    "-Wno-unknown-warning-option",
    "-DNOMINMAX",
    "-Wno-deprecated-declarations",
    "-Wno-implicit-int-conversion",
    "-Wno-missing-prototypes",
    "-Wno-missing-variable-declarations",
    "-Wno-shadow",
    "-Wno-shorten-64-to-32",
    "-Wno-sign-compare",
    "-Wno-sign-conversion",
    "-Wno-unreachable-code-loop-increment",
    "-Wno-unused-function",
    "-Wno-unused-member-function",
    "-Wno-unused-parameter",
    "-Wno-unused-private-field",
    "-Wno-unused-template",
    "-Wno-used-but-marked-unused",
    "-Wno-gnu-zero-variadic-macro-arguments",
]
