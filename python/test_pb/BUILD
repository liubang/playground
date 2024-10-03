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

load("@rules_python//python:defs.bzl", "py_binary")
load("@rules_python//python:proto.bzl", "py_proto_library")

py_proto_library(
    name = "py_sensor_proto",
    deps = [
        "//proto/test:sensor",
    ],
)

py_binary(
    name = "sensor_pb",
    srcs = ["sensor_pb.py"],
    deps = [":py_sensor_proto"],
)
