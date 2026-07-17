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
# Created: 2026/07/17 22:27

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//cpp:copts/configure_copts.bzl", "TEST_COPTS")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "ordered_uint_test",
    srcs = ["ordered_uint_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/sstv2/codec:ordered_uint",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "cell_key_codec_test",
    srcs = ["cell_key_codec_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        "//cpp/pl/minitable/codec:cell_key_codec",
        "//cpp/pl/sstv2/types:schema",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)
