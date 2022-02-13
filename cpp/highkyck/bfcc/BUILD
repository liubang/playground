package(
    default_visibility = ["//visibility:public"],
)

_COMMON_COPTS = [
    "-std=c++17",
    "-fsanitize=address",
    "-fno-omit-frame-pointer",
]

_COMMON_LINKOPTS = [
    "-fsanitize=address",
]

cc_library(
    name = "diagnostic",
    srcs = [
        "diagnostic.cpp",
    ],
    hdrs = [
        "diagnostic.h",
    ],
    copts = _COMMON_COPTS,
)

cc_library(
    name = "lexer",
    srcs = [
        "lexer.cpp",
    ],
    hdrs = [
        "lexer.h",
    ],
    copts = _COMMON_COPTS,
    deps = [
        ":diagnostic",
    ],
)

cc_library(
    name = "parser",
    srcs = [
        "ast_node.cpp",
        "parser.cpp",
    ],
    hdrs = [
        "ast_node.h",
        "parser.h",
    ],
    copts = _COMMON_COPTS,
    deps = [
        ":lexer",
        ":diagnostic",
    ],
)

cc_library(
    name = "print_visitor",
    srcs = [
        "print_visitor.cpp",
    ],
    hdrs = [
        "print_visitor.h",
    ],
    copts = _COMMON_COPTS,
    deps = [
        ":parser",
    ],
)

cc_library(
    name = "codegen",
    srcs = [
        "codegen.cpp",
    ],
    hdrs = [
        "codegen.h",
    ],
    copts = _COMMON_COPTS,
    deps = [
        ":parser",
    ],
)

cc_binary(
    name = "bfcc",
    srcs = ["bfcc.cpp"],
    copts = _COMMON_COPTS,
    linkopts = _COMMON_LINKOPTS,
    deps = [
        ":lexer",
        ":parser",
        ":codegen",
    ],
)
