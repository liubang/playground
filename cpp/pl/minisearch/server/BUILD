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
# Created: 2026/08/17

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
    "TEST_COPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "search_pipeline",
    srcs = ["search_pipeline.cpp"],
    hdrs = ["search_pipeline.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minisearch:faiss_index",
        "//cpp/pl/minisearch/analysis:analyzer",
        "//cpp/pl/minisearch/core",
        "//cpp/pl/minisearch/index:inverted",
    ],
)

cc_test(
    name = "search_pipeline_test",
    srcs = ["ut/search_pipeline_test.cc"],
    copts = TEST_COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":search_pipeline",
        "//cpp/pl/minisearch:faiss_index",
        "//cpp/pl/minisearch/analysis:analyzer",
        "//cpp/pl/minisearch/core",
        "//cpp/pl/minisearch/index:inverted",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "codec",
    srcs = ["codec.cpp"],
    hdrs = ["codec.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minisearch/analysis:analyzer",
        "//cpp/pl/minisearch/core",
        "//cpp/pl/minisearch/proto:minisearch_cc_proto",
    ],
)

cc_library(
    name = "filter",
    srcs = ["filter.cpp"],
    hdrs = ["filter.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/minisearch/core",
        "//cpp/pl/minisearch/proto:minisearch_cc_proto",
    ],
)

cc_library(
    name = "markdown",
    srcs = ["markdown.cpp"],
    hdrs = ["markdown.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
)

cc_library(
    name = "static_files",
    srcs = ["static_files.cpp"],
    hdrs = ["static_files.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "@brpc",
    ],
)

cc_library(
    name = "server",
    srcs = [
        "collection_registry.cpp",
        "context.cpp",
        "http_api.cpp",
    ],
    hdrs = [
        "collection_registry.h",
        "context.h",
        "http_api.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":codec",
        ":filter",
        ":markdown",
        ":search_pipeline",
        ":static_files",
        "//cpp/pl/minisearch:embedding_client",
        "//cpp/pl/minisearch:faiss_index",
        "//cpp/pl/minisearch:rerank_client",
        "//cpp/pl/minisearch/analysis:analyzer",
        "//cpp/pl/minisearch/auth:console_auth",
        "//cpp/pl/minisearch/auth:keystore",
        "//cpp/pl/minisearch/core",
        "//cpp/pl/minisearch/index:inverted",
        "//cpp/pl/minisearch/proto:minisearch_cc_proto",
        "//cpp/pl/minisearch/storage:checkpoint",
        "@brpc",
    ],
)

cc_test(
    name = "http_api_test",
    srcs = ["ut/http_api_test.cc"],
    copts = TEST_COPTS,
    # protobuf RepeatedField reads trip libc++ container-overflow annotations
    # under ASan (false positive); disable that check for this test only.
    env = {"ASAN_OPTIONS": "detect_container_overflow=0"},
    linkopts = LINKOPTS,
    deps = [
        ":server",
        "//cpp/pl/minisearch/auth:console_auth",
        "//cpp/pl/minisearch/auth:keystore",
        "//cpp/pl/minisearch/core",
        "@brpc",
        "@googletest//:gtest_main",
    ],
)
