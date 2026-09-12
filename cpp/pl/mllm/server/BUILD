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
# Created: 2026/09/12

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_proto_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS", "TEST_COPTS")

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "mllm_proto",
    srcs = ["mllm.proto"],
)

cpp_proto_library(
    name = "mllm_cc_proto",
    protos = [":mllm_proto"],
)

cc_library(
    name = "server_config",
    hdrs = ["server_config.h"],
    copts = COPTS,
)

cc_library(
    name = "http_util",
    srcs = ["http_util.cpp"],
    hdrs = ["http_util.h"],
    copts = COPTS,
    deps = [
        ":server_config",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/engine",
        "@simdjson",
    ],
)

cc_library(
    name = "http_service",
    srcs = ["http_service.cpp"],
    hdrs = ["http_service.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":http_util",
        ":mllm_cc_proto",
        ":server_config",
        "//cpp/pl/mllm/engine",
        "//cpp/pl/mllm/media:image_io",
        "@brpc",
        "@simdjson",
    ],
)

cc_library(
    name = "log_sink",
    srcs = ["log_sink.cpp"],
    hdrs = ["log_sink.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "@brpc",
    ],
)

cc_binary(
    name = "mllm_server",
    srcs = ["server_main.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        ":http_service",
        ":log_sink",
        "//cpp/pl/mllm/engine",
        "@brpc",
        "@gflags",
    ],
)

cc_test(
    name = "http_util_test",
    srcs = ["ut/http_util_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        ":http_util",
        "//cpp/pl/mllm/core",
        "@googletest//:gtest_main",
        "@simdjson",
    ],
)

cc_test(
    name = "log_sink_test",
    srcs = ["ut/log_sink_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        ":log_sink",
        "@googletest//:gtest_main",
    ],
)

# End-to-end: tiny hermetic GGUF model + real brpc server + HTTP channel.
cc_test(
    name = "http_service_test",
    srcs = ["ut/http_service_test.cpp"],
    copts = TEST_COPTS,
    deps = [
        ":http_service",
        ":http_util",
        ":server_config",
        "//cpp/pl/mllm/engine",
        "//cpp/pl/mllm/ut/testdata:gguf_writer",
        "@brpc",
        "@googletest//:gtest_main",
        "@simdjson",
    ],
)
