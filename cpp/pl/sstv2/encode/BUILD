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
# Created: 2026/06/04 12:01

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "varints",
    srcs = ["varints.cpp"],
    hdrs = ["varints.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/sstv2/types:data_type"],
)

cc_library(
    name = "stream_vbyte",
    srcs = ["stream_vbyte.cpp"],
    hdrs = ["stream_vbyte.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [":varints"],
)

cc_library(
    name = "fixed",
    hdrs = ["fixed.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/sstv2/types:data_type"],
)
