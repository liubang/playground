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

load("@rules_cc//cc:defs.bzl", "cc_binary")
load(
    "//cpp:copts/configure_copts.bzl",
    "COPTS",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

cc_binary(
    name = "setjmp_impl",
    srcs = ["setjmp_impl.cpp"],
    copts = ["-std=c++20"] + COPTS,
    deps = [
        "//cpp/pl/lang",
    ],
)

cc_binary(
    name = "context_impl",
    srcs = ["context_impl.cpp"],
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
)
