package(default_visibility = ["//visibility:public"])

cc_library(
    name = "common",
    hdrs = [
        "cli_table.h",
        "print.h",
    ],
)

cc_test(
    name = "cli_table_test",
    srcs = [
        "cli_table_test.cpp",
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
        ":common",
        "@gtest//:gtest_main",
    ],
)
