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

# Architecture registry: supported families and their feature flags.
cc_library(
    name = "architecture",
    hdrs = ["architecture.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
)

# Header-only hyperparameter set shared by loader and model runtime.
cc_library(
    name = "config",
    hdrs = ["config.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/model:architecture",
    ],
)

# Weight name patterns for dense decoder GGUF models.
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

# Abstract Model interface (header-only; split out so concrete
# implementations can depend on it without a factory cycle).
cc_library(
    name = "model_interface",
    hdrs = ["model.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model:config",
    ],
)

# Model factory (CreateModel dispatches on architecture).
cc_library(
    name = "model",
    srcs = ["model_factory.cpp"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/model:architecture",
        "//cpp/pl/mllm/model:dense_decoder",
        "//cpp/pl/mllm/model:model_interface",
    ],
)

# Dense decoder-only implementation shared by llama/qwen2/qwen3-style families.
cc_library(
    name = "dense_decoder",
    srcs = ["dense_decoder.cpp"],
    hdrs = ["dense_decoder.h"],
    copts = COPTS,
    linkopts = LINKOPTS,
    deps = [
        "//cpp/pl/mllm/backend",
        "//cpp/pl/mllm/core",
        "//cpp/pl/mllm/kv_cache",
        "//cpp/pl/mllm/model:config",
        "//cpp/pl/mllm/model:model_interface",
        "//cpp/pl/mllm/model:transformer_layer",
        "//cpp/pl/mllm/model:weight_names",
    ],
)
