# Copyright (c) 2021 The Authors. All rights reserved.
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
# Created: 2021/07/04 00:28

_COPTS = [
    "-std=gnu++17",
]

cc_binary(
    name = "factorial_test",
    srcs = [
        "factorial.h",
        "factorial_test.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "forward",
    srcs = [
        "forward.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "forward2",
    srcs = [
        "forward2.cc",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "deducing_type_test",
    srcs = [
        "deducing_type.h",
        "deducing_type_test.cpp",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "is_nothrow_xxxx_constructible",
    srcs = [
        "is_nothrow_xxxx_constructible.cc",
    ],
    copts = _COPTS,
)

cc_library(
    name = "common",
    hdrs = [
        "common.h",
    ],
    copts = _COPTS,
)

cc_library(
    name = "scope_guard",
    hdrs = ["scope_guard.h"],
    deps = [
        ":common",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "scope_guard_test",
    srcs = [
        "scope_guard_test.cpp",
    ],
    copts = _COPTS,
    deps = [
        ":scope_guard",
    ],
)

cc_binary(
    name = "common_test",
    srcs = [
        "common_test.cpp",
        "common.h",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "enable_if_test",
    srcs = [
        "enable_if.h",
        "enable_if_test.cpp",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "fibonacci_test",
    srcs = [
        "fibonacci.h",
        "fibonacci_test.cpp",
    ],
    copts = _COPTS,
)

cc_binary(
    name = "compute_type_test",
    srcs = [
        "compute_type.h",
        "compute_type_test.cpp",
    ],
    copts = _COPTS,
)
