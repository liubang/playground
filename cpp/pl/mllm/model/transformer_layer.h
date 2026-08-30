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

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/model/config.h"

namespace pl::mllm {

// Weight references for one transformer layer.
// MatMul weights are identified by name (backend handles dequantization);
// elementwise weights (norm) are passed as TensorView since any dtype can be
// read via the backend's element access.
struct LayerWeights {
    // Attention projection — passed by name to backend.MatMul
    std::string_view q_weight_name;
    std::string_view k_weight_name;
    std::string_view v_weight_name;
    std::string_view o_weight_name;
    // MLP projection
    std::string_view gate_weight_name;
    std::string_view up_weight_name;
    std::string_view down_weight_name;
    // Norm weights — passed as TensorView
    TensorView attn_norm;
    TensorView mlp_norm;
    // Optional additive Q/K/V projection biases (Qwen2). Invalid TensorView
    // = absent; sizes are [heads*head_dim] / [kv_heads*head_dim].
    TensorView q_bias;
    TensorView k_bias;
    TensorView v_bias;
    // Optional per-head Q/K RMSNorm weights applied before RoPE (Qwen3).
    // Shape: [head_dim] each. Invalid TensorView = absent.
    TensorView q_norm;
    TensorView k_norm;
};

// One transformer layer. See SPEC §6.2.
class TransformerLayer {
public:
    TransformerLayer(int32_t layer_index, LayerWeights weights)
        : layer_index_(layer_index), weights_(weights) {}

    // Forward pass for a single token.
    // hidden: [1, hidden_size] — modified in-place (residual adds).
    // position: absolute position in the sequence (for RoPE).
    // cache: shared KV cache; appends K/V for this layer.
    // scratch: arena for intermediate activations (reset per token by caller).
    Status Forward(TensorView hidden,
                   int64_t position,
                   KVCache& cache,
                   Backend& backend,
                   ScratchArena& scratch,
                   const ModelConfig& config) const;

private:
    int32_t layer_index_;
    LayerWeights weights_;
};

} // namespace pl::mllm
