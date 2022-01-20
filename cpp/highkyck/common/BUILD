package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "cli_table_test",
    srcs = [
        "cli_table.h",
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
)
