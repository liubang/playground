// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/29 22:15

#pragma once

#include <cstdint>
#include <string>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm {

// Decoder-only, LLaMA-compatible model hyperparameters.
struct ModelConfig {
    std::string architecture;
    int32_t vocab_size = 0;
    int32_t hidden_size = 0;
    int32_t intermediate_size = 0;
    int32_t num_layers = 0;
    int32_t num_attention_heads = 0;
    int32_t num_kv_heads = 0;
    int32_t head_dim = 0;
    int32_t context_length = 0;
    float rms_norm_eps = 1e-5f;
    float rope_freq_base = 10000.0f;

    [[nodiscard]] Status Validate() const {
        if (architecture != "llama") {
            return Status::Error(ErrorCode::kUnsupported,
                                 "unsupported architecture: " + architecture);
        }
        if (vocab_size <= 0 || hidden_size <= 0 || intermediate_size <= 0 || num_layers <= 0 ||
            num_attention_heads <= 0 || num_kv_heads <= 0 || context_length <= 0) {
            return Status::Error(ErrorCode::kInvalidFormat, "config: non-positive dimension");
        }
        const int32_t hd = head_dim != 0 ? head_dim : hidden_size / num_attention_heads;
        if (hidden_size != num_attention_heads * hd) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: hidden_size != heads * head_dim");
        }
        if (hd % 2 != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: head_dim must be even for RoPE");
        }
        if (num_attention_heads % num_kv_heads != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: heads not divisible by kv_heads");
        }
        if (!(rms_norm_eps > 0.0f) || !(rope_freq_base > 0.0f)) {
            return Status::Error(ErrorCode::kInvalidFormat, "config: bad float field");
        }
        return {};
    }

    [[nodiscard]] int32_t effective_head_dim() const {
        return head_dim != 0 ? head_dim : hidden_size / num_attention_heads;
    }
};

} // namespace pl::mllm
