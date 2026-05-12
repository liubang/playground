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

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

cc_library(
    name = "arena",
    srcs = [
        "arena_old.cpp",
    ],
    hdrs = [
        "arena.h",
        "arena_old.h",
    ],
    copts = [
    ] + COPTS,
    linkopts = LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/utility",
    ],
)

cc_test(
    name = "arena_test",
    srcs = [
        "arena_test.cpp",
    ],
    copts = [
    ] + TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/arena",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "arena_old_test",
    srcs = [
        "arena_old_test.cpp",
    ],
    copts = [
    ] + TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/arena",
        "@googletest//:gtest_main",
    ],
)
