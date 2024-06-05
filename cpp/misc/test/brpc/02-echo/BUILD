# Copyright (c) 2024 The Authors. All rights reserved.
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

load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "echo_pb",
    srcs = ["echo.proto"],
)

cc_proto_library(
    name = "cc_echo_pb",
    deps = [":echo_pb"],
)

cc_binary(
    name = "echo_server",
    srcs = [
        "echo_server.cpp",
        "echo_service.cpp",
        "echo_service.h",
    ],
    copts = ["-std=c++17"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        ":cc_echo_pb",
        "@brpc",
    ],
)
