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

load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

cc_library(
    name = "engine",
    srcs = ["engine.cpp"],
    hdrs = ["engine.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/backend/cpu:cpu_backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/loader",
        "//cpp/pl/mllm/model:llama_model",
        "//cpp/pl/mllm/sampler",
        "//cpp/pl/mllm/tokenizer",
    ] + select({
        "@platforms//os:macos": ["//cpp/pl/mllm/backend/metal:metal_backend"],
        "//conditions:default": [],
    }),
)
