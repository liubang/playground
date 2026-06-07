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
# Created: 2026/06/06 14:11

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "tail",
    srcs = ["tail.cpp"],
    hdrs = ["tail.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/codec:checksum",
        "//cpp/pl/sstv2/codec:fixed",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "section",
    srcs = ["section.cpp"],
    hdrs = ["section.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/codec:checksum",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/codec:varint",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "metadata",
    srcs = ["metadata.cpp"],
    hdrs = ["metadata.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":section",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/types:internal_schema",
        "//cpp/pl/sstv2/types:schema",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)
