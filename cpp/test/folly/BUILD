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
# Created: 2020/11/19 16:22

# package(features = ["fully_static_link"])

_copts = [
    "-std=gnu++17",
    "-Wall",
    "-Wextra",
    "-Wcast-qual",
    "-Wconversion-null",
    "-Woverlength-strings",
    "-Wpointer-arith",
    "-Wno-unused-local-typedefs",
    "-Wno-unused-parameter",
    "-Wunused-result",
    "-Wvarargs",
    "-Wvla",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-Wno-sign-compare",
    "-msse3",
    "-msse4",
    "-msse4.1",
    "-mpclmul",
    "-fno-builtin-memcmp",
]

cc_binary(
    name = "scope",
    srcs = [
        "scope.cc",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)
cc_binary(
    name = "future",
    srcs = [
        "future.cc",
    ],
    linkopts = [
        "-l:libstdc++.a",
    ],
    features = [
        # "fully_static_link",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)

cc_binary(
    name = "singleton",
    srcs = [
        "singleton.cc",
        "singleton.h",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)

cc_binary(
    name = "test1",
    srcs = [
        "test1.cc",
    ],
    copts = _copts,
    deps = [
        "@folly",
    ],
)
