# Copyright (c) 2022 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Authors: liubang (it.liubang@gmail.com)
# Created: 2022/02/13 16:21

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
    cmd = "./$(locations :bfcc) 'a=3;a<=3;' > $@",
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
    cmd = "./$(locations :bfcc) 'a=2; if (a!=3) {a = 3; a = a * a;} else {if (a == 3) a = 5; else a = 6;} a = a + 7;' > $@",
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
    cmd = "./$(locations :bfcc) 'a=1;while(a<10){a=a+1;}a;' > $@",
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
