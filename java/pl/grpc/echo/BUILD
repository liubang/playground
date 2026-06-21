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
# Created: 2026/06/21

load("@rules_java//java:defs.bzl", "java_binary")

java_binary(
    name = "echo_server",
    srcs = ["EchoServer.java"],
    main_class = "pl.grpc.echo.EchoServer",
    deps = [
        "//proto/echo:echo_java_grpc",
        "//proto/echo:echo_java_proto",
        "@maven//:io_grpc_grpc_api",
        "@maven//:io_grpc_grpc_netty",
        "@maven//:io_grpc_grpc_stub",
    ],
)

java_binary(
    name = "echo_client",
    srcs = ["EchoClient.java"],
    main_class = "pl.grpc.echo.EchoClient",
    deps = [
        "//proto/echo:echo_java_grpc",
        "//proto/echo:echo_java_proto",
        "@maven//:io_grpc_grpc_api",
        "@maven//:io_grpc_grpc_netty",
        "@maven//:io_grpc_grpc_stub",
    ],
)
