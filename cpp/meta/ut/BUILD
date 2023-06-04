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
# Created: 2023/05/21 02:32

#======================================================================
#
# BUILD -
#
# Created by liubang on 2023/05/21 00:59
# Last Modified: 2023/05/21 00:59
#
#======================================================================

load("//cpp:opts.bzl", "common_copts", "common_linkopts")

cc_test(
    name = "expression_test",
    srcs = [
        "expression_test.cpp",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "traits_test",
    srcs = [
        "traits_test.cpp",
        "traits_test.h",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "utils_test",
    srcs = [
        "utils_test.cpp",
        "utils_test.h",
    ],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "tuple_iteration_basics_test",
    srcs = ["tuple_iteration_basics_test.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "tuple_iteration_apply_test",
    srcs = ["tuple_iteration_apply_test.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "type_list_test",
    srcs = ["type_list_test.cpp"],
    copts = common_copts + ["-std=c++20"],
    linkopts = common_linkopts,
    deps = [
        "//cpp/meta",
        "@googletest//:gtest_main",
    ],
)
