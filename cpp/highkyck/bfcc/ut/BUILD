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
    name = "gen_test_stmt",
    srcs = [":bfcc"],
    outs = ["gen_test_stmt.s"],
    cmd = "./$(locations :bfcc) 'test() {a=3;a<=3;}' > $@",
)

cc_binary(
    name = "test_stmt",
    srcs = [
        "driver.c",
        ":gen_test_stmt",
    ],
    copts = [
        "-std=c11",
    ],
)

genrule(
    name = "gen_test_ifelse",
    srcs = [":bfcc"],
    outs = ["gen_test_ifelse.s"],
    cmd = "./$(locations :bfcc) 'test() {a=2; if (a!=3) {a = 3; a = a * a;} else {if (a == 3) a = 5; else a = 6;} a = a + 7;}' > $@",
)

cc_binary(
    name = "test_ifelse",
    srcs = [
        "driver.c",
        ":gen_test_ifelse",
    ],
    copts = [
        "-std=c11",
    ],
)

genrule(
    name = "gen_while",
    srcs = [":bfcc"],
    outs = ["gen_while.s"],
    cmd = "./$(locations :bfcc) 'test() {a=1;while(a<10){a=a+1;}a;}' > $@",
)

cc_binary(
    name = "test_while",
    srcs = [
        "driver.c",
        ":gen_while",
    ],
    copts = [
        "-std=c11",
    ],
)

genrule(
    name = "gen_do_while",
    srcs = [":bfcc"],
    outs = ["gen_do_while.s"],
    cmd = "./$(locations :bfcc) 'test() {i=1;j=1;do{i = i + 1; j=j+i;} while (i < 10);j;}' > $@",
)

cc_binary(
    name = "test_do_while",
    srcs = [
        "driver.c",
        ":gen_do_while",
    ],
    copts = [
        "-std=c11",
    ],
)

genrule(
    name = "gen_for_loop",
    srcs = [":bfcc"],
    outs = ["gen_for_loop.s"],
    cmd = "./$(locations :bfcc) 'test() {j=0;for(i=0;i <=10;i=i+1) {j=j+i;};j;}' > $@",
)

cc_binary(
    name = "test_for_loop",
    srcs = [
        "driver.c",
        ":gen_for_loop",
    ],
    copts = [
        "-std=c11",
    ],
)

genrule(
    name = "gen_function_call",
    srcs = [":bfcc"],
    outs = ["gen_function_call.s"],
    cmd = "./$(locations :bfcc) 'sum(n) {n = n + 10;n;} test() {b = sum(12); b;}' > $@",
)

cc_binary(
    name = "test_function_call",
    srcs = [
        "driver.c",
        ":gen_function_call",
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
