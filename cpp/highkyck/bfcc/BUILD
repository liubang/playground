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

cc_test(
    name = "lexer_test",
    srcs = [
        "lexer.cpp",
        "lexer.h",
        "ut/lexer_test.cpp",
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
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "codegen_test",
    srcs = [
        "lexer.cpp",
        "lexer.h",
        "parser.h",
        "parser.cpp",
        "ast_node.h",
        "ast_node.cpp",
        "codegen.h",
        "codegen.cpp",
        "print_visitor.h",
        "print_visitor.cpp",
        "ut/codegen_test.cpp",
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
        "@gtest//:gtest_main",
    ],
)
