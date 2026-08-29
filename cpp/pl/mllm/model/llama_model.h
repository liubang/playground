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

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/arena.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/kv_cache/kv_cache.h"
#include "cpp/pl/mllm/model/config.h"
#include "cpp/pl/mllm/model/transformer_layer.h"

namespace pl::mllm {

// Full LLaMA-compatible decoder model.
// Owns transformer layers and top-level weight views.
class LlamaModel {
public:
    // Build from a config and a set of named weight views (resolved from GGUF).
    // The caller must keep the backing storage (mmap / GGUFFile) alive.
    struct WeightEntry {
        std::string name;
        TensorView view;
    };

    [[nodiscard]] static Result<std::unique_ptr<LlamaModel>> Create(
        ModelConfig config, std::span<const WeightEntry> weights);

    // Forward one token through all layers.
    // hidden: [1, hidden_size] — input embedding (caller owns it).
    // position: absolute sequence position.
    // cache: KV cache (appends all layers, then advances).
    // scratch: arena (caller resets per token).
    // backend: compute backend with weights already imported.
    Status Forward(TensorView hidden,
                   int64_t position,
                   KVCache& cache,
                   Backend& backend,
                   ScratchArena& scratch) const;

    // Final norm + output projection (lm_head).
    // hidden: [1, hidden_size] — output of last layer.
    // logits: [1, vocab_size] — output.
    Status ComputeLogits(TensorView hidden,
                         TensorView logits,
                         Backend& backend,
                         ScratchArena& scratch) const;

    [[nodiscard]] const ModelConfig& config() const noexcept { return config_; }
    [[nodiscard]] int32_t num_layers() const noexcept {
        return static_cast<int32_t>(layers_.size());
    }

    // Weight names that the backend must import.
    [[nodiscard]] std::vector<std::string> weight_names() const;

private:
    LlamaModel() = default;

    ModelConfig config_;
    std::vector<TransformerLayer> layers_;
    // Top-level weights
    TensorView output_norm_;              // output_norm.weight
    std::string_view output_weight_name_; // output.weight (or tied)
    bool tied_output_ = false;            // if true, output shares token_embd.weight
    // Per-layer weight name strings (kept alive for string_view references).
    std::vector<std::string> name_storage_;
};

} // namespace pl::mllm
