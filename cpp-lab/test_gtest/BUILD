load(
    "//:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
)

cc_library(
    name = "math",
    hdrs = [
        "math.h",
    ],
)

cc_test(
    name = "test_math",
    srcs = [
        "test_math.cc",
    ],
    deps = [
        ":math",
        "@gtest//:gtest_main",
    ],
)
