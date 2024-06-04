# Copyright (c) 2023 The Authors. All rights reserved.
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

# load("@rules_ragel//ragel:ragel.bzl", "ragel")
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

# ragel(
#     name = "scanner_generated",
#     src = "scanner.rl",
#     data = [
#         "unicode.rl",
#     ],
#     language = "c++",
#     ragel_options = ["-I ."],
# )

cc_library(
    name = "scanner",
    srcs = [
        "scanner.cpp",
        "scanner_generated.cc",
    ],
    hdrs = [
        "scanner.h",
        "token.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)

cc_binary(
    name = "scanner_test",
    srcs = [
        "scanner_test.cpp",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":scanner",
    ],
)

cc_library(
    name = "parser",
    srcs = [
        "ast.cpp",
        "parser.cpp",
        "strconv.cpp",
    ],
    hdrs = [
        "ast.h",
        "parser.h",
        "strconv.h",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    local_defines = ["FLUX_ENABLE_DEBUG"],
    deps = [
        ":scanner",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_binary(
    name = "parser_test",
    srcs = [
        "parser_test.cpp",
    ],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":parser",
    ],
)
