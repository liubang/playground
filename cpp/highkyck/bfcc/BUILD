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
    srcs = [
        "bfcc.cpp",
        "lexer.h",
        "lexer.cpp",
        "parser.h",
        "parser.cpp",
        "ast_node.h",
        "ast_node.cpp",
        "print_visitor.h",
        "print_visitor.cpp",
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
