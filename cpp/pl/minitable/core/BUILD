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
    name = "slice_apply_machine",
    srcs = ["slice_apply_machine.cpp"],
    hdrs = ["slice_apply_machine.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":key",
        ":slice_store",
        "//cpp/pl/minitable/codec:cell_key_codec",
        "//cpp/pl/minitable/proto/v2:internal_cc_proto",
        "//cpp/pl/sstv2/types:value",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "vessel_slice_state_machine",
    srcs = ["vessel_slice_state_machine.cpp"],
    hdrs = ["vessel_slice_state_machine.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":slice_apply_machine",
        "//cpp/pl/minitable/codec:cell_key_codec",
        "//cpp/pl/minivessel:replica_runtime",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "vessel_slice_replica",
    srcs = ["vessel_slice_replica.cpp"],
    hdrs = ["vessel_slice_replica.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":vessel_slice_state_machine",
        "//cpp/pl/minivessel:replica_runtime",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "slice_proposal_frontend",
    srcs = ["slice_proposal_frontend.cpp"],
    hdrs = ["slice_proposal_frontend.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":braft_slice_adapter",
        ":data_service_core",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@braft//:braft",
    ],
)

cc_library(
    name = "slice_snapshot",
    srcs = ["slice_snapshot.cpp"],
    hdrs = ["slice_snapshot.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":slice_apply_machine",
        ":slice_store",
        "//cpp/pl/minitable/proto/v2:internal_cc_proto",
        "//cpp/pl/sstv2/codec:fixed",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "slice_raft_state_machine",
    srcs = ["slice_raft_state_machine.cpp"],
    hdrs = ["slice_raft_state_machine.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":slice_apply_machine",
        ":slice_snapshot",
        "//cpp/pl/minitable/codec:cell_key_codec",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "braft_slice_adapter",
    srcs = ["braft_slice_adapter.cpp"],
    hdrs = ["braft_slice_adapter.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":slice_raft_state_machine",
        "@braft//:braft",
    ],
)

cc_library(
    name = "data_service_core",
    srcs = ["data_service_core.cpp"],
    hdrs = ["data_service_core.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":embedded_slice",
        ":slice_apply_machine",
        "//cpp/pl/minitable/codec:cell_key_codec",
        "//cpp/pl/minitable/proto/v2:data_cc_proto",
        "//cpp/pl/sstv2/types:value",
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
