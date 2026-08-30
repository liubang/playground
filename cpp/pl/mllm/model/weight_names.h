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

#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/model/config.h"

namespace pl::mllm {

// Weight name patterns for dense decoder models (GGUF naming convention,
// shared by llama / qwen2 / qwen3 / ...). Optional tensors (QKV bias, QK
// norm) are only populated when the corresponding feature is enabled.
struct LayerWeightNames {
    // Attention
    std::string q_weight; // token_embd.weight ... etc. Set per-layer.
    std::string k_weight;
    std::string v_weight;
    std::string o_weight;
    // Optional QKV biases (Qwen2); empty when not applicable.
    std::string q_bias;
    std::string k_bias;
    std::string v_bias;
    // Optional per-head Q/K RMSNorm weights (Qwen3); empty when not applicable.
    std::string q_norm;
    std::string k_norm;
    // Attention norm
    std::string attn_norm;
    // MLP
    std::string gate_weight; // ffn_gate
    std::string up_weight;   // ffn_up
    std::string down_weight; // ffn_down
    // MLP norm
    std::string mlp_norm;
};

// Build standard GGUF weight names for layer `layer`.
// GGUF convention: blk.{layer}.{suffix}
// Optional names follow the arch feature flags (see ArchSpec).
inline LayerWeightNames make_layer_weight_names(int32_t layer,
                                                bool qkv_bias = false,
                                                bool qk_norm = false) {
    const std::string prefix = "blk." + std::to_string(layer) + ".";
    return LayerWeightNames{
        .q_weight = prefix + "attn_q.weight",
        .k_weight = prefix + "attn_k.weight",
        .v_weight = prefix + "attn_v.weight",
        .o_weight = prefix + "attn_output.weight",
        .q_bias = qkv_bias ? prefix + "attn_q.bias" : "",
        .k_bias = qkv_bias ? prefix + "attn_k.bias" : "",
        .v_bias = qkv_bias ? prefix + "attn_v.bias" : "",
        .q_norm = qk_norm ? prefix + "attn_q_norm.weight" : "",
        .k_norm = qk_norm ? prefix + "attn_k_norm.weight" : "",
        .attn_norm = prefix + "attn_norm.weight",
        .gate_weight = prefix + "ffn_gate.weight",
        .up_weight = prefix + "ffn_up.weight",
        .down_weight = prefix + "ffn_down.weight",
        .mlp_norm = prefix + "ffn_norm.weight",
    };
}

// Standard top-level weight names.
inline std::string token_embedding_name() {
    return "token_embd.weight";
}
inline std::string output_norm_name() {
    return "output_norm.weight";
}
// output projection: may be tied to token embedding or separate
inline std::string output_weight_name() {
    return "output.weight";
}

} // namespace pl::mllm
