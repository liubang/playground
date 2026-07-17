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
# Created: 2026/07/17 22:28

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//cpp/pl/minitable:__subpackages__"])

cc_library(
    name = "manifest",
    srcs = ["manifest.cpp"],
    hdrs = ["manifest.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":sst_read_source",
        "//cpp/pl/sstv2/codec:fixed",
        "//cpp/pl/sstv2/io:filesystem",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "embedded_slice",
    srcs = ["embedded_slice.cpp"],
    hdrs = ["embedded_slice.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":key",
        ":slice_store",
        "//cpp/pl/minitable/codec:cell_key_codec",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "sst_read_source",
    srcs = ["sst_read_source.cpp"],
    hdrs = ["sst_read_source.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/sstv2/file:sstable",
        "//cpp/pl/sstv2/io:filesystem",
        "//cpp/pl/sstv2/merge:merge_iterator",
        "//cpp/pl/sstv2/types:key",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "slice_store",
    srcs = ["slice_store.cpp"],
    hdrs = ["slice_store.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":manifest",
        ":sst_read_source",
        "//cpp/pl/minitable/memtable",
        "//cpp/pl/sstv2/file:sstable",
        "//cpp/pl/sstv2/io:filesystem",
        "//cpp/pl/sstv2/merge:merge_iterator",
        "//cpp/pl/sstv2/types:key",
        "//cpp/pl/sstv2/types:row",
        "//cpp/pl/sstv2/types:schema",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "key",
    hdrs = ["key.h"],
    deps = [
        "//cpp/pl/sstv2/types:op_type",
        "//cpp/pl/sstv2/types:value",
    ],
)
