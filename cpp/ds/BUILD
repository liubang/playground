_TEST_COPTS = [
    "-std=gnu++2a",
    "-Wno-conversion-null",
    "-Wno-deprecated-declarations",
    "-Wno-sign-compare",
    "-Wno-unused-function",
    "-Wno-unused-parameter",
    "-Wno-unused-private-field",
]

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
    copts = _TEST_COPTS,
    deps = [
        "@gtest//:gtest_main",
    ],
)
