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
# Created: 2026/06/04 13:06

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "pattern_encoder",
    hdrs = ["pattern_encoder.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["@abseil-cpp//absl/status"],
)

cc_library(
    name = "pattern_decoder_base",
    hdrs = ["pattern_decoder.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [":pattern_encoder"],
)

cc_library(
    name = "pattern_none",
    srcs = ["pattern_none.cpp"],
    hdrs = ["pattern_none.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:fixed",
    ],
)

cc_library(
    name = "pattern_constant",
    srcs = ["pattern_constant.cpp"],
    hdrs = ["pattern_constant.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "pattern_delta",
    srcs = ["pattern_delta.cpp"],
    hdrs = ["pattern_delta.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "pattern_stream_vbyte",
    srcs = ["pattern_stream_vbyte.cpp"],
    hdrs = ["pattern_stream_vbyte.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:stream_vbyte",
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "pattern_dict",
    srcs = ["pattern_dict.cpp"],
    hdrs = ["pattern_dict.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "pattern_pfor",
    srcs = ["pattern_pfor.cpp"],
    hdrs = ["pattern_pfor.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_decoder_base",
        ":pattern_encoder",
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
    ],
)

cc_library(
    name = "pattern_decoder",
    srcs = ["pattern_decoder.cpp"],
    hdrs = ["pattern_decoder.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_constant",
        ":pattern_decoder_base",
        ":pattern_delta",
        ":pattern_dict",
        ":pattern_encoder",
        ":pattern_none",
        ":pattern_pfor",
        ":pattern_stream_vbyte",
    ],
)

cc_library(
    name = "pattern_selector",
    srcs = ["pattern_selector.cpp"],
    hdrs = ["pattern_selector.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":pattern_constant",
        ":pattern_delta",
        ":pattern_dict",
        ":pattern_encoder",
        ":pattern_none",
        ":pattern_pfor",
        ":pattern_stream_vbyte",
    ],
)
