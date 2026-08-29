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

#include "cpp/pl/mllm/model/llama_model.h"

#include <unordered_map>

namespace pl::mllm {

namespace {

// Find a weight view by name; returns empty view if not found.
TensorView find_weight(std::span<const LlamaModel::WeightEntry> weights, std::string_view name) {
    for (const auto& w : weights) {
        if (w.name == name) {
            return w.view;
        }
    }
    return {};
}

} // namespace

Result<std::unique_ptr<LlamaModel>> LlamaModel::Create(ModelConfig config,
                                                       std::span<const WeightEntry> weights) {
    if (auto s = config.Validate(); !s.ok()) {
        return s;
    }

    auto model = std::unique_ptr<LlamaModel>(new LlamaModel());
    model->config_ = config;

    // Resolve output norm weight
    model->output_norm_ = find_weight(weights, "output_norm.weight");

    // Reserve enough capacity so string_views stay valid (no reallocation).
    // 1 (output) + 6 (per-layer: q/k/v/o/gate/up/down) * num_layers
    model->name_storage_.reserve(1 + 7 * static_cast<size_t>(config.num_layers));

    // Resolve output weight (may be tied to token_embd.weight)
    TensorView output_w = find_weight(weights, "output.weight");
    if (output_w.valid()) {
        model->tied_output_ = false;
        model->name_storage_.push_back("output.weight");
        model->output_weight_name_ = model->name_storage_.back();
    } else {
        model->tied_output_ = true;
        model->name_storage_.push_back("token_embd.weight");
        model->output_weight_name_ = model->name_storage_.back();
    }

    // Build per-layer weight references
    for (int32_t l = 0; l < config.num_layers; ++l) {
        // Build standard GGUF names
        auto make_name = [l](std::string_view suffix) {
            return "blk." + std::to_string(l) + "." + std::string(suffix);
        };

        LayerWeights lw;

        // Store weight names in name_storage_ for stable string_views
        model->name_storage_.push_back(make_name("attn_q.weight"));
        lw.q_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("attn_k.weight"));
        lw.k_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("attn_v.weight"));
        lw.v_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("attn_output.weight"));
        lw.o_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("ffn_gate.weight"));
        lw.gate_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("ffn_up.weight"));
        lw.up_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(make_name("ffn_down.weight"));
        lw.down_weight_name = model->name_storage_.back();

        // Norm weights — resolved as TensorView
        lw.attn_norm = find_weight(weights, make_name("attn_norm.weight"));
        lw.mlp_norm = find_weight(weights, make_name("ffn_norm.weight"));

        if (!lw.attn_norm.valid() || !lw.mlp_norm.valid()) {
            return Status::Error(ErrorCode::kNotFound,
                                 "missing norm weight for layer " + std::to_string(l));
        }

        model->layers_.emplace_back(l, lw);
    }

    return model;
}

Status LlamaModel::Forward(TensorView hidden,
                           int64_t position,
                           KVCache& cache,
                           Backend& backend,
                           ScratchArena& scratch) const {
    for (size_t l = 0; l < layers_.size(); ++l) {
        if (auto s = layers_[l].Forward(hidden, position, cache, backend, scratch, config_);
            !s.ok()) {
            return s;
        }
    }
    // All layers have appended; advance the cache position
    cache.Advance();
    return {};
}

Status LlamaModel::ComputeLogits(TensorView hidden,
                                 TensorView logits,
                                 Backend& backend,
                                 ScratchArena& scratch) const {
    // Final RMSNorm
    auto norm_out = scratch.AllocateTensor({1, config_.hidden_size}, DType::kF32);
    if (!norm_out.ok())
        return norm_out.status();
    auto norm = norm_out.value();

    if (auto s = backend.RmsNorm(norm, hidden, output_norm_, config_.rms_norm_eps); !s.ok()) {
        return s;
    }

    // Output projection: logits = norm @ output_weight^T
    // logits shape: [1, vocab_size]
    if (auto s = backend.MatMul(logits, norm, output_weight_name_); !s.ok()) {
        return s;
    }

    return {};
}

std::vector<std::string> LlamaModel::weight_names() const {
    std::vector<std::string> names;
    // Output weight
    names.emplace_back(output_weight_name_);
    // Output norm
    names.emplace_back("output_norm.weight");
    // Per-layer
    for (int32_t l = 0; l < config_.num_layers; ++l) {
        auto prefix = "blk." + std::to_string(l) + ".";
        for (auto suffix : {"attn_q.weight",
                            "attn_k.weight",
                            "attn_v.weight",
                            "attn_output.weight",
                            "attn_norm.weight",
                            "ffn_gate.weight",
                            "ffn_up.weight",
                            "ffn_down.weight",
                            "ffn_norm.weight"}) {
            names.push_back(prefix + suffix);
        }
    }
    // Token embedding (always needed, even if tied)
    names.emplace_back("token_embd.weight");
    return names;
}

} // namespace pl::mllm
