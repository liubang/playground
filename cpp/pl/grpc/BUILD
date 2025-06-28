# Copyright (c) 2025 The Authors. All rights reserved.
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

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "LINKOPTS",
)

cc_binary(
    name = "echo_service",
    srcs = [
        "echo_service.cpp",
        "echo_service_impl.cpp",
        "echo_service_impl.h",
    ],
    copts = COPTS + ["-std=c++20"],
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/grpc/proto:grpc_echo_pb",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)

cc_binary(
    name = "echo_client",
    srcs = [
        "echo_client.cpp",
        "echo_client.h",
    ],
    copts = COPTS + ["-std=c++20"],
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/grpc/proto:grpc_echo_pb",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)
