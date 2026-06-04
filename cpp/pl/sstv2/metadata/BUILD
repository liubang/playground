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
# Created: 2026/06/04 15:23

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "metadata_section",
    srcs = ["metadata_section.cpp"],
    hdrs = ["metadata_section.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/encode:fixed",
        "//cpp/pl/sstv2/encode:varints",
        "@abseil-cpp//absl/crc:crc32c",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "configuration",
    srcs = ["configuration.cpp"],
    hdrs = ["configuration.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":metadata_section",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "schema_meta",
    srcs = ["schema_meta.cpp"],
    hdrs = ["schema_meta.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":metadata_section",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "statistics",
    srcs = ["statistics.cpp"],
    hdrs = ["statistics.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":metadata_section",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "compatibility",
    srcs = ["compatibility.cpp"],
    hdrs = ["compatibility.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":metadata_section",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "user_defined",
    srcs = ["user_defined.cpp"],
    hdrs = ["user_defined.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":metadata_section",
        "@abseil-cpp//absl/status:statusor",
    ],
)
