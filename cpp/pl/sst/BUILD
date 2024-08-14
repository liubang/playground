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

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")
load(
    "//cpp:copts/configure_copts.bzl",
    "DEFAULT_COPTS",
    "DEFAULT_LINKOPTS",
)

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "sstable",
    srcs = glob(["*.cpp"]),
    hdrs = glob(["*.h"]),
    copts = ["-std=c++20"] + DEFAULT_COPTS,
    linkopts = DEFAULT_LINKOPTS,
    deps = [
        "//cpp/pl/arena",
        "//cpp/pl/bits",
        "//cpp/pl/bloom:bloom_filter",
        "//cpp/pl/crc",
        "//cpp/pl/fs",
        "//cpp/pl/log:logger",
        "//cpp/pl/random",
        "//cpp/pl/scope",
        "@snappy",
        "@zstd",
    ],
)

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = {
        "//cpp/pl/sst/...": "",
    },
)
