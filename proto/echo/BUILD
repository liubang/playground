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

load("@protobuf//bazel:java_proto_library.bzl", "java_proto_library")
load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_grpc_library")
load("@rules_proto_grpc_java//:defs.bzl", "java_grpc_library")
load("@rules_proto_grpc_python//:defs.bzl", "python_grpc_library", "python_proto_library")

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "echo_proto",
    srcs = ["echo.proto"],
)

# C++
cpp_grpc_library(
    name = "echo_cc_grpc",
    protos = [":echo_proto"],
)

# Java
java_proto_library(
    name = "echo_java_proto",
    deps = [":echo_proto"],
)

java_grpc_library(
    name = "echo_java_grpc",
    protos = [":echo_proto"],
)

# Python
python_proto_library(
    name = "echo_py_proto",
    protos = [":echo_proto"],
)

python_grpc_library(
    name = "echo_py_grpc",
    protos = [":echo_proto"],
)
