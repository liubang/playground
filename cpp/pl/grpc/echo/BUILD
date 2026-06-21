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
# Created: 2026/06/21 00:00

load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

cc_binary(
    name = "echo_server",
    srcs = [
        "echo_server.cpp",
        "echo_service_impl.cpp",
        "echo_service_impl.h",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//proto/echo:echo_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
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
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//proto/echo:echo_cc_grpc",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:initialize",
        "@grpc//:grpc++",
    ],
)
