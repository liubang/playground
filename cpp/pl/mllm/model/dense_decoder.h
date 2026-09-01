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
#include "cpp/pl/mllm/model/model.h"
#include "cpp/pl/mllm/model/transformer_layer.h"

namespace pl::mllm {

// Dense decoder-only model shared by all "llama-like" families in the arch
// registry (llama / qwen2 / qwen3, ...). Family differences (QKV bias, QK
// norm, RoPE base frequency, decoupled head_dim) are driven by ModelConfig
// feature flags populated from the GGUF metadata; see architecture.h.
class DenseDecoderModel : public Model {
public:
    // Build from a config and a set of named weight views (resolved from
    // GGUF). The caller must keep the backing storage alive.
    [[nodiscard]] static Result<std::unique_ptr<DenseDecoderModel>> Create(
        ModelConfig config, std::span<const Model::WeightEntry> weights);

    Status Forward(TensorView hidden,
                   int64_t position,
                   KVCache& cache,
                   Backend& backend,
                   ScratchArena& scratch) const override;

    Status Prefill(TensorView hidden,
                   int64_t start_pos,
                   KVCache& cache,
                   Backend& backend,
                   ScratchArena& scratch) const override;

    Status ComputeLogits(TensorView hidden,
                         TensorView logits,
                         Backend& backend,
                         ScratchArena& scratch) const override;

    [[nodiscard]] const ModelConfig& config() const noexcept override { return config_; }
    [[nodiscard]] int32_t num_layers() const noexcept override {
        return static_cast<int32_t>(layers_.size());
    }

    [[nodiscard]] std::vector<std::string> weight_names() const override;

private:
    DenseDecoderModel() = default;

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
