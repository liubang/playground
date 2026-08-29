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

// Weight name patterns for LLaMA-compatible models (GGUF naming convention).
struct LayerWeightNames {
    // Attention
    std::string q_weight; // token_embd.weight ... etc. Set per-layer.
    std::string k_weight;
    std::string v_weight;
    std::string o_weight;
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
inline LayerWeightNames make_layer_weight_names(int32_t layer) {
    const std::string prefix = "blk." + std::to_string(layer) + ".";
    return LayerWeightNames{
        .q_weight = prefix + "attn_q.weight",
        .k_weight = prefix + "attn_k.weight",
        .v_weight = prefix + "attn_v.weight",
        .o_weight = prefix + "attn_output.weight",
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
