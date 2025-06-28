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

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS_WITH_ASAN",
    "LINKOPTS_WITH_ASAN",
    "TEST_COPTS",
)

cc_library(
    name = "hash",
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + COPTS_WITH_ASAN,
    linkopts = LINKOPTS_WITH_ASAN,
    visibility = ["//visibility:public"],
)

# cc_test(
#     name = "murmurhash2_test",
#     srcs = [
#         "murmurhash2_test.cpp",
#     ],
#     copts = [
#         "-std=c++20",
#     ] + TEST_COPTS,
#     linkopts = LINKOPTS_WITH_ASAN,
#     deps = [
#         "//cpp/pl/hash",
#         "@googletest//:gtest_main",
#     ],
# )
