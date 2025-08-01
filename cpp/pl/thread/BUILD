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

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS_WITH_ASAN",
    "LINKOPTS_WITH_ASAN",
    "TEST_COPTS",
)

cc_library(
    name = "thread_pool",
    hdrs = ["thread_pool.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "thread",
    srcs = ["thread.cpp"],
    hdrs = ["thread.h"],
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN + ["-pthread"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "thread_pool_test",
    srcs = ["thread_pool_test.cpp"],
    copts = ["-std=c++17"] + TEST_COPTS,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":thread_pool",
        "//cpp/pl/log:logger",
        "@googletest//:gtest_main",
    ],
)
