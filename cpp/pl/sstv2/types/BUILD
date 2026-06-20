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
# Created: 2026/06/05 00:23

load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "data_type",
    hdrs = ["data_type.h"],
)

cc_library(
    name = "op_type",
    hdrs = ["op_type.h"],
)

cc_library(
    name = "value",
    srcs = ["value.cpp"],
    hdrs = ["value.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":data_type",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/codec:varint",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "schema",
    hdrs = ["schema.h"],
    deps = [":data_type"],
)

cc_library(
    name = "internal_schema",
    hdrs = ["internal_schema.h"],
    deps = [":schema"],
)

cc_library(
    name = "row",
    hdrs = ["row.h"],
    deps = [
        ":key",
        ":op_type",
        ":value",
    ],
)

cc_library(
    name = "internal_row",
    hdrs = ["internal_row.h", "column_flag.h"],
    deps = [
        ":internal_schema",
        ":value",
    ],
)

cc_library(
    name = "key",
    hdrs = ["key.h", "key_comparator.h", "key_factory.h", "key_prefix.h"],
    deps = [
        ":internal_row",
        ":internal_schema",
        ":op_type",
        ":schema",
        ":value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)
