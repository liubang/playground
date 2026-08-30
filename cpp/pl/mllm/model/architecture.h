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
// Created: 2026/08/30

#pragma once

#include <array>
#include <string_view>

namespace pl::mllm {

// Static description of a supported model architecture (GGUF
// `general.architecture` value). Per-family structural differences are
// expressed as feature flags so that one dense decoder implementation can
// cover a whole family, and genuinely different families (MoE, MLA, ...)
// can opt out via `dense_decoder == false` and plug in their own `Model`
// implementation in the model factory.
struct ArchSpec {
    // GGUF architecture name; also the metadata key prefix ("llama.", ...).
    std::string_view name;
    // Whether this family is implemented by DenseDecoderModel. When false,
    // the model factory dispatches to a dedicated implementation.
    bool dense_decoder = true;
    // Additive bias on Q/K/V projections (e.g. Qwen2: attn_q.bias, ...).
    bool qkv_bias = false;
    // Per-head RMSNorm on Q/K before RoPE
    // (e.g. Qwen3: attn_q_norm.weight / attn_k_norm.weight).
    bool qk_norm = false;
    // RoPE base frequency used when the GGUF metadata omits rope.freq_base.
    float default_rope_freq_base = 10000.0f;
};

// Registry of supported architectures. Add new families here.
inline constexpr ArchSpec kArchLlama{.name = "llama"};
inline constexpr ArchSpec kArchQwen2{
    .name = "qwen2",
    .qkv_bias = true,
    .default_rope_freq_base = 1000000.0f,
};
inline constexpr ArchSpec kArchQwen3{
    .name = "qwen3",
    .qk_norm = true,
    .default_rope_freq_base = 1000000.0f,
};

inline constexpr std::array kSupportedArchitectures{
    &kArchLlama,
    &kArchQwen2,
    &kArchQwen3,
};

// Look up an architecture by GGUF name; nullptr when unsupported.
inline const ArchSpec* find_architecture(std::string_view name) noexcept {
    for (const ArchSpec* spec : kSupportedArchitectures) {
        if (spec->name == name) {
            return spec;
        }
    }
    return nullptr;
}

} // namespace pl::mllm
