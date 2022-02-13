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
# Created: 2022/01/17 00:02

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
