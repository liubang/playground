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

#include <array>
#include <cstdint>
#include <string>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/model/architecture.h"

namespace pl::mllm {

// Decoder-only model hyperparameters, shared by all supported dense decoder
// families (llama, qwen2, qwen3, ...). Family-specific behavior is toggled by
// the feature flags populated from the architecture registry
// (see architecture.h); the GGUF loader resolves and stores them here.
struct ModelConfig {
    std::string architecture;
    int32_t vocab_size = 0;
    int32_t hidden_size = 0;
    int32_t intermediate_size = 0;
    int32_t num_layers = 0;
    int32_t num_attention_heads = 0;
    int32_t num_kv_heads = 0;
    // Explicit per-head dim. 0 = derive as hidden_size / num_attention_heads.
    // May differ from hidden_size / heads (e.g. Qwen3 uses 128 per head with
    // hidden_size 1024 and 16 heads).
    int32_t head_dim = 0;
    int32_t context_length = 0;
    float rms_norm_eps = 1e-5f;
    float rope_freq_base = 10000.0f;
    // Feature flags (see ArchSpec): additive Q/K/V bias (Qwen2), additive
    // attention-output bias (ERNIE 4.5) and per-head Q/K RMSNorm (Qwen3).
    bool qkv_bias = false;
    bool o_bias = false;
    bool qk_norm = false;

    // Multimodal rotary (MRoPE, Qwen2-VL family): split of the head_dim/2
    // rotary pairs into (temporal, height, width) sections, e.g. {16, 24,
    // 24} for head_dim 128. All zeros = plain 1D rope (text-only models).
    // Parsed from `<arch>.rope.mrope_section` in the GGUF metadata.
    std::array<int32_t, 3> mrope_section{0, 0, 0};

    [[nodiscard]] bool has_mrope() const noexcept { return mrope_section[0] > 0; }

    [[nodiscard]] Status Validate() const {
        if (find_architecture(architecture) == nullptr) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "unsupported architecture: " + architecture);
        }
        if (vocab_size <= 0 || hidden_size <= 0 || intermediate_size <= 0 || num_layers <= 0 ||
            num_attention_heads <= 0 || num_kv_heads <= 0 || context_length <= 0) {
            return Status::Error(ErrorCode::kInvalidFormat, "config: non-positive dimension");
        }
        const int32_t hd = head_dim != 0 ? head_dim : hidden_size / num_attention_heads;
        if (head_dim == 0 && hidden_size != num_attention_heads * hd) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: hidden_size not divisible by heads");
        }
        if (hd <= 0 || hd % 2 != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: head_dim must be positive and even for RoPE");
        }
        if (num_attention_heads % num_kv_heads != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "config: heads not divisible by kv_heads");
        }
        if (!(rms_norm_eps > 0.0f) || !(rope_freq_base > 0.0f)) {
            return Status::Error(ErrorCode::kInvalidFormat, "config: bad float field");
        }
        if (has_mrope()) {
            const int64_t sum =
                static_cast<int64_t>(mrope_section[0]) + mrope_section[1] + mrope_section[2];
            const bool positive =
                mrope_section[0] > 0 && mrope_section[1] > 0 && mrope_section[2] > 0;
            if (!positive || sum != hd / 2) {
                return Status::Error(ErrorCode::kInvalidFormat,
                                     "config: mrope_section must be positive and sum to "
                                     "head_dim/2");
            }
        }
        return {};
    }

    [[nodiscard]] int32_t effective_head_dim() const {
        return head_dim != 0 ? head_dim : hidden_size / num_attention_heads;
    }
};

} // namespace pl::mllm
