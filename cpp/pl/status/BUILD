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
    "LINKOPTS_WITH_ASAN",
    "TEST_COPTS",
)

cc_library(
    name = "status",
    srcs = [
        "status_code.cpp",
    ],
    hdrs = [
        "result.h",
        "status.h",
        "status_code.h",
        "status_code_detail.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@folly//folly:expected",
        "@folly//folly:unit",
        "@folly//folly/logging",
    ],
)

cc_test(
    name = "status_test",
    srcs = ["status_test.cpp"],
    copts = ["-std=c++20"] + TEST_COPTS,
    linkopts = LINKOPTS_WITH_ASAN,
    deps = [
        ":status",
        "@googletest//:gtest_main",
    ],
)
