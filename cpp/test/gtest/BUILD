_copts = [
    "-std=gnu++17",
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
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

cc_library(
    name = "math",
    hdrs = [
        "math.h",
    ],
    copts = _copts,
)

cc_test(
    name = "test_math",
    srcs = [
        "test_math.cc",
    ],
    copts = _copts,
    deps = [
        ":math",
        "@gtest//:gtest_main",
    ],
)
