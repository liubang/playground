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
    visibility = ["//visibility:public"],
)

cc_test(
    name = "test_arena",
    srcs = [
        "arena_test.cpp",
    ],
    copts = [
        "-std=c++17",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
    ],
    linkopts = [
        "-fsanitize=address",
    ],
    deps = [
        ":arena",
        "@gtest//:gtest_main",
    ],
)
