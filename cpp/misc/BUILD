# Copyright (c) 2020 The Authors. All rights reserved.
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
# Created: 2020/07/20 00:32

cc_binary(
    name = "test_signal",
    srcs = ["test_signal.cc"],
)

cc_binary(
    name = "test_oop",
    srcs = [
        "test_oop.cc",
    ],
)

cc_binary(
    name = "test_file_info",
    srcs = [
        "test_file_info.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_binary(
    name = "test_string",
    srcs = [
        "test_string.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_library(
    name = "plugin1",
    srcs = [
        "dlopen/plugin1.cc",
    ],
    hdrs = [
        "dlopen/singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_library(
    name = "plugin2",
    srcs = [
        "dlopen/plugin1.cc",
    ],
    hdrs = [
        "dlopen/singleton.h",
    ],
    copts = [
        "-std=gnu++14",
    ],
)

cc_binary(
    name = "test_dlopen",
    srcs = [
        "dlopen/main.cc",
    ],
    copts = [
        "-std=gnu++14",
    ],
    linkopts = [
        "-ldl",
    ],
)

cc_binary(
    name = "context",
    srcs = [
        "context.cc",
    ],
    copts = [
        "-std=gnu++17",
    ],
)

cc_binary(
    name = "test_hash",
    srcs = [
        "test_hash.cc",
        "hash.h",
        "encoding.h",
    ],
    copts = [
        "-std=gnu++17",
    ],
)

cc_binary(
    name = "named_parameters",
    srcs = [
        "named_parameters.cc",
    ],
    copts = [
        "-std=gnu++2a",
    ],
    deps = [
        "@boost//:headers",
    ],
)

cc_binary(
    name = "partial_function",
    srcs = [
        "partial_function.cc",
    ],
    copts = [
        "-std=gnu++2a",
    ],
)

cc_binary(
    name = "partial_application",
    srcs = [
        "partial_application.cc",
    ],
    copts = [
        "-std=gnu++2a",
    ],
)

cc_binary(
    name = "curry_function",
    srcs = [
        "curry_function.cc",
    ],
    copts = [
        "-std=gnu++2a",
    ],
)
