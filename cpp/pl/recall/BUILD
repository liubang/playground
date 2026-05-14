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
# Created: 2026/05/14 10:44

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

cc_library(
    name = "faiss_index",
    srcs = ["faiss_index.cpp"],
    hdrs = ["faiss_index.h"],
    copts = COPTS,
    deps = [
        "@faiss",
    ],
)

cc_library(
    name = "embedding_client",
    srcs = ["embedding_client.cpp"],
    hdrs = ["embedding_client.h"],
    copts = COPTS,
    deps = [
        "@brpc",
    ],
)

cc_library(
    name = "recall_service_impl",
    srcs = ["recall_service.cpp"],
    hdrs = ["recall_service.h"],
    copts = COPTS,
    deps = [
        ":embedding_client",
        ":faiss_index",
        "//cpp/pl/recall/proto:recall_cc_proto",
        "@brpc",
    ],
)

cc_binary(
    name = "recall_server",
    srcs = ["recall_server.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":recall_service_impl",
        "@brpc",
        "@gflags",
    ],
)

cc_binary(
    name = "recall_client",
    srcs = ["recall_client.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "@brpc",
        "@gflags",
    ],
)
