cc_library(
    name = "arena",
    srcs = [
        "arena.cpp",
    ],
    hdrs = [
        "arena.h",
    ],
    copts = [
        "-std=c++17",
    ],
)

cc_test(
    name = "test_arena",
    srcs = [
        "arena_test.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        ":arena",
        "@gtest//:gtest_main",
    ],
)
