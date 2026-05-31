# Copyright (c) 2026 The Authors. All rights reserved.
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
# Created: 2026/05/10 23:13

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//cpp/pl/flux:__subpackages__"])

cc_library(
    name = "optimizer",
    srcs = [
        "cbo.cpp",
        "explain.cpp",
        "rbo.cpp",
    ],
    hdrs = [
        "cbo.h",
        "explain.h",
        "rbo.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/flux/connector",
        "//cpp/pl/flux/plan",
        "//cpp/pl/flux/runtime:runtime_value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:strings",
    ],
)

cc_test(
    name = "cbo_unit_test",
    srcs = [
        "tests/cbo_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":optimizer",
        "@googletest//:gtest_main",
        "@sqlite3",
    ],
)

cc_test(
    name = "rbo_unit_test",
    srcs = [
        "tests/rbo_unit_test.cpp",
    ],
    copts = COPTS,
    data = [
        "//cpp/pl/flux/examples:cross_source_data",
    ],
    linkopts = LINKOPTS,
    deps = [
        ":optimizer",
        "@googletest//:gtest_main",
    ],
)
