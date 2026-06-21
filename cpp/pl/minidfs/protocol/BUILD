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
# Created: 2025/06/30 00:47

load("@rules_proto_grpc_cpp//:defs.bzl", "cpp_proto_library")

package(default_visibility = ["//cpp/pl/minidfs:__subpackages__"])

proto_library(
    name = "minidfs_proto",
    srcs = ["minidfs.proto"],
)

cpp_proto_library(
    name = "minidfs_cc_proto",
    protos = [":minidfs_proto"],
)
