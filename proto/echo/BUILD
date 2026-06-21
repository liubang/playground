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

load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:java_proto_library.bzl", "java_proto_library")
load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_grpc_library")
load("@rules_proto_grpc_go//:defs.bzl", "go_grpc_library")
load("@rules_proto_grpc_java//:defs.bzl", "java_grpc_library")
load("@rules_proto_grpc_python//:defs.bzl", "python_grpc_library", "python_proto_library")

package(default_visibility = ["//visibility:public"])

# --- Messages (shared by gRPC + brpc) ---

proto_library(
    name = "message_proto",
    srcs = ["message.proto"],
)

cc_proto_library(
    name = "message_cc_proto",
    deps = [":message_proto"],
)

java_proto_library(
    name = "message_java_proto",
    deps = [":message_proto"],
)

python_proto_library(
    name = "message_py_proto",
    protos = [":message_proto"],
)

# --- gRPC: EchoService (unary) ---

proto_library(
    name = "echo_proto",
    srcs = ["echo.proto"],
    deps = [":message_proto"],
)

# --- gRPC: StreamService (streaming) ---

proto_library(
    name = "stream_proto",
    srcs = ["stream.proto"],
    deps = [":message_proto"],
)

# --- brpc: EchoService (unary, cc_generic_services=true) ---

proto_library(
    name = "echo_brpc_proto",
    srcs = ["echo_brpc.proto"],
    deps = [":message_proto"],
)

cc_proto_library(
    name = "echo_brpc_cc_proto",
    deps = [":echo_brpc_proto"],
)

# === gRPC codegen ===
# message_proto must be listed first so its generated message classes
# are available to the service stubs in echo_proto and stream_proto.

ALL_GRPC_PROTOS = [
    ":message_proto",
    ":echo_proto",
    ":stream_proto",
]

# C++ gRPC — each target includes message_proto for transitive include paths
cpp_grpc_library(
    name = "echo_cc_grpc",
    protos = [
        ":message_proto",
        ":echo_proto",
    ],
)

cpp_grpc_library(
    name = "stream_cc_grpc",
    protos = [
        ":message_proto",
        ":stream_proto",
    ],
)

# Java
java_proto_library(
    name = "echo_java_proto",
    deps = ALL_GRPC_PROTOS,
)

java_grpc_library(
    name = "echo_java_grpc",
    protos = ALL_GRPC_PROTOS,
)

# Python
python_proto_library(
    name = "echo_py_proto",
    protos = ALL_GRPC_PROTOS,
)

python_grpc_library(
    name = "echo_py_grpc",
    protos = ALL_GRPC_PROTOS,
)

# Go
go_grpc_library(
    name = "echo_go_grpc",
    importpath = "github.com/liubang/playground/go/pl/grpc/echo/pb",
    protos = ALL_GRPC_PROTOS,
)
