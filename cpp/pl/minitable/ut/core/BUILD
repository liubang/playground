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
# Created: 2026/07/18 00:36

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//cpp:copts/configure_copts.bzl", "LINKOPTS", "TEST_COPTS")

cc_test(
    name = "embedded_slice_test",
    srcs = ["embedded_slice_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minitable/core:embedded_slice",
        "//cpp/pl/sstv2/io:local_filesystem",
        "//cpp/pl/sstv2/types:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "slice_store_test",
    srcs = ["slice_store_test.cpp"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minitable/core:slice_store",
        "//cpp/pl/minitable/memtable",
        "//cpp/pl/sstv2/io:local_filesystem",
        "@googletest//:gtest_main",
    ],
)
