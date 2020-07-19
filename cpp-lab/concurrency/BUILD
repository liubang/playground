COPTS_COMMON = [
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Wmissing-declarations",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wno-unused-local-typedefs",
    "-Wno-unused-parameter",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-msse3",
    "-msse4",
    "-msse4.1",
    "-mpclmul",
    "-fno-builtin-memcmp",
]

C11_OPTS = [
    "-std=gnu++11",
] + COPTS_COMMON

C14_OPTS = [
    "-std=gnu++14",
] + COPTS_COMMON

C2A_OPTS = [
    "-std=gnu++2a",
] + COPTS_COMMON

cc_binary(
    name = "multithread-1",
    srcs = ["multithread-1.cc"],
    copts = C14_OPTS,
    linkopts = ["-pthread"],
)
