# Copyright (c) 2024 The Authors. All rights reserved.
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

load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_test(
    name = "expression_test",
    srcs = [
        "expression_test.cpp",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "traits_test",
    srcs = [
        "traits_test.cpp",
        "traits_test.h",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "utils_test",
    srcs = [
        "utils_test.cpp",
        "utils_test.h",
    ],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "tuple_iteration_apply_test",
    srcs = ["tuple_iteration_apply_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

# cc_test(
#     name = "tuple_iteration_basics_test",
#     srcs = ["tuple_iteration_basics_test.cpp"],
#     copts = ["-std=c++20"] + TEST_COPTS,
#     linkopts = DEFAULT_LINKOPTS,
#     deps = [
#         "//cpp/meta",
#         "@catch2//:catch2_main",
#     ],
# )

cc_test(
    name = "type_list_test",
    srcs = ["type_list_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "function_test",
    srcs = ["function_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)

cc_test(
    name = "unique_ptr_test",
    srcs = ["unique_ptr_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/meta",
        "@catch2//:catch2_main",
    ],
)
