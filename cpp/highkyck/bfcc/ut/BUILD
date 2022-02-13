_COMMON_COPTS = [
    "-std=c++17",
    "-fsanitize=address",
    "-fno-omit-frame-pointer",
]

_COMMON_LINKOPTS = [
    "-fsanitize=address",
]

cc_binary(
    name = "bfcc",
    srcs = [
        "bfcc.cpp",
    ],
    copts = _COMMON_COPTS,
    linkopts = _COMMON_LINKOPTS,
    deps = [
        "//highkyck/bfcc:lexer",
        "//highkyck/bfcc:parser",
        "//highkyck/bfcc:codegen",
    ],
)
genrule(
    name = "compiled_source",
    srcs = [":bfcc"],
    outs = ["compiled_source.s"],
    cmd = "./$(locations :bfcc) 'a=123;b=4; a * b;' > $@",
)

cc_binary(
    name = "driver",
    srcs = [
        "driver.c",
        ":compiled_source",
    ],
    copts = [
        "-std=c11",
    ],
)

cc_test(
    name = "lexer_test",
    srcs = [
        "lexer_test.cpp",
    ],
    copts = _COMMON_COPTS,
    linkopts = _COMMON_LINKOPTS,
    deps = [
        "//highkyck/bfcc:lexer",
        "@gtest//:gtest_main",
    ],
)

cc_test(
    name = "print_visitor_test",
    srcs = [
        "print_visitor_test.cpp",
    ],
    copts = _COMMON_COPTS,
    linkopts = _COMMON_LINKOPTS,
    deps = [
        "//highkyck/bfcc:print_visitor",
        "@gtest//:gtest_main",
    ],
)
