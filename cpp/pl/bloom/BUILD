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
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "bloom_filter",
    srcs = ["bloom_filter.cpp"],
    hdrs = ["bloom_filter.h"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/binary",
        "//cpp/pl/hash",
        "//cpp/pl/random",
        "//cpp/pl/scope",
    ],
)

cc_test(
    name = "bloom_filter_test",
    srcs = [
        "bloom_filter_test.cpp",
    ],
    copts = [
        "-std=c++20",
    ] + TEST_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/pl/bloom:bloom_filter",
        "@googletest//:gtest_main",
    ],
)
