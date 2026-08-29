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
# Created: 2026/08/29 22:15

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

# End-to-end decode benchmark (SPEC §13.1): prefill/decode throughput, tok/s,
# peak RSS, backend, git commit.
cc_binary(
    name = "bench_decode",
    srcs = [
        "bench_common.h",
        "bench_decode.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/engine",
        "//cpp/pl/mllm/loader",
    ],
)

# Per-kernel operator benchmark (SPEC §13.2): RMSNorm / GEMV / Attention across
# representative shapes. Run with `--backend cpu|metal`.
cc_binary(
    name = "bench_ops",
    srcs = [
        "bench_common.h",
        "bench_ops.cpp",
    ],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/backend/cpu:cpu_backend",
        "//cpp/pl/mllm/core",
    ] + select({
        "@platforms//os:macos": ["//cpp/pl/mllm/backend/metal:metal_backend"],
        "//conditions:default": [],
    }),
)
