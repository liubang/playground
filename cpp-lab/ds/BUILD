load(
    "//:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "TEST_COPTS",
)

cc_test(
    name = "ds",
    srcs = [
        "common.h",
        "000-merge-sort.cc",
        "155-min-stack.cc",
        "289-game-of-life.cc",
        "300-longest-increasing-subsequence.cc",
        "452-minimum-number-of-arrowsto-burst-balloons.cc",
    ],
    copts = TEST_COPTS,
    deps = [
        "@gtest//:gtest_main",
    ],
)

