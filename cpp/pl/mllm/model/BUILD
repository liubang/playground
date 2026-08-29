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

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//cpp:copts/configure_copts.bzl", "COPTS", "LINKOPTS")

package(default_visibility = ["//visibility:public"])

# Header-only hyperparameter set shared by loader and model runtime.
cc_library(
    name = "config",
    hdrs = ["config.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/mllm/core"],
)

# Weight name patterns for LLaMA GGUF models.
cc_library(
    name = "weight_names",
    hdrs = ["weight_names.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = ["//cpp/pl/mllm/core"],
)

# One transformer decoder layer.
cc_library(
    name = "transformer_layer",
    srcs = ["transformer_layer.cpp"],
    hdrs = ["transformer_layer.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model:config",
    ],
)

# Full LLaMA model.
cc_library(
    name = "llama_model",
    srcs = ["llama_model.cpp"],
    hdrs = ["llama_model.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model:config",
        "//cpp/pl/mllm/model:transformer_layer",
    ],
)
