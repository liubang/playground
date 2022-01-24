cc_test(
    name = "lexer_test",
    srcs = [
        "lexer.cpp",
        "lexer.h",
        "ut/lexer_test.cpp",
    ],
    copts = [
        "-std=c++17",
    ],
    deps = [
        "@gtest//:gtest_main",
    ],
)

cc_binary(
    name = "bfcc",
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    copts = [
        "-std=c++17",
        "-fsanitize=address",
        "-fno-omit-frame-pointer",
    ],
    linkopts = [
        "-fsanitize=address",
    ],
)
