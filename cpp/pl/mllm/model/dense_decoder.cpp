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

#include "cpp/pl/mllm/model/dense_decoder.h"

#include "cpp/pl/mllm/model/weight_names.h"

namespace pl::mllm {

namespace {

// Find a weight view by name; returns empty view if not found.
TensorView find_weight(std::span<const Model::WeightEntry> weights, std::string_view name) {
    for (const auto& w : weights) {
        if (w.name == name) {
            return w.view;
        }
    }
    return {};
}

const char* dtype_label(DType d) {
    switch (d) {
        case DType::kF32:
            return "f32";
        case DType::kF16:
            return "f16";
        case DType::kQ4_0:
            return "q4_0";
        case DType::kQ8_0:
            return "q8_0";
    }
    return "unknown";
}

// Dtypes with end-to-end support in every backend's MatMul path.
bool is_supported_matmul_dtype(DType d) {
    return d == DType::kF32 || d == DType::kF16 || d == DType::kQ8_0 || d == DType::kQ4_0;
}

// Norms / biases are consumed elementwise (RmsNorm, AddBiasInPlace) and must
// be dense floats; a block-quantized dtype here would read garbage (CPU
// elem_to_f32 yields 0 for Q4_0 -> silent all-zero activations).
bool is_supported_elementwise_dtype(DType d) {
    return d == DType::kF32 || d == DType::kF16;
}

// Fail fast on missing or unsupported weights so a broken checkpoint errors
// at load time with the tensor name, instead of surfacing deep inside the
// first forward pass (or, worse, silently producing wrong activations).
Status require_matmul_weight(std::span<const Model::WeightEntry> weights, std::string_view name) {
    const TensorView w = find_weight(weights, name);
    if (!w.valid()) {
        return Status::Error(ErrorCode::kNotFound, "missing weight: " + std::string(name));
    }
    if (!is_supported_matmul_dtype(w.dtype())) {
        return Status::Error(ErrorCode::kUnsupported,
                             "unsupported dtype " + std::string(dtype_label(w.dtype())) +
                                 " for weight: " + std::string(name));
    }
    return {};
}

Status require_elementwise_weight(const TensorView& w, std::string_view name) {
    if (!is_supported_elementwise_dtype(w.dtype())) {
        return Status::Error(ErrorCode::kUnsupported,
                             "unsupported dtype " + std::string(dtype_label(w.dtype())) +
                                 " for weight: " + std::string(name));
    }
    return {};
}

} // namespace

Result<std::unique_ptr<DenseDecoderModel>> DenseDecoderModel::Create(
    ModelConfig config, std::span<const Model::WeightEntry> weights) {
    if (auto s = config.Validate(); !s.ok()) {
        return s;
    }

    auto model = std::unique_ptr<DenseDecoderModel>(new DenseDecoderModel());
    model->config_ = config;

    // Resolve output norm weight
    model->output_norm_ = find_weight(weights, "output_norm.weight");
    if (!model->output_norm_.valid()) {
        return Status::Error(ErrorCode::kNotFound, "missing weight: output_norm.weight");
    }
    if (auto s = require_elementwise_weight(model->output_norm_, "output_norm.weight"); !s.ok()) {
        return s;
    }

    // Reserve enough capacity so string_views stay valid (no reallocation).
    // 1 (output) + up to 7 (per-layer matmul names) * num_layers
    model->name_storage_.reserve(1 + 8 * static_cast<size_t>(config.num_layers));

    // Resolve output weight (may be tied to token_embd.weight)
    TensorView output_w = find_weight(weights, "output.weight");
    if (output_w.valid()) {
        model->tied_output_ = false;
        if (!is_supported_matmul_dtype(output_w.dtype())) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "unsupported dtype " + std::string(dtype_label(output_w.dtype())) +
                                     " for weight: output.weight");
        }
        model->name_storage_.push_back("output.weight");
        model->output_weight_name_ = model->name_storage_.back();
    } else {
        model->tied_output_ = true;
        model->name_storage_.push_back("token_embd.weight");
        model->output_weight_name_ = model->name_storage_.back();
    }
    // token_embd.weight always participates (embedding + possibly tied output).
    if (auto s = require_matmul_weight(weights, "token_embd.weight"); !s.ok()) {
        return s;
    }

    // Build per-layer weight references from the standard GGUF naming scheme,
    // honoring the family feature flags (Qwen2 bias, Qwen3 QK norm).
    for (int32_t l = 0; l < config.num_layers; ++l) {
        const LayerWeightNames names = make_layer_weight_names(l, config.qkv_bias, config.qk_norm);

        LayerWeights lw;
        model->name_storage_.push_back(names.q_weight);
        lw.q_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.k_weight);
        lw.k_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.v_weight);
        lw.v_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.o_weight);
        lw.o_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.gate_weight);
        lw.gate_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.up_weight);
        lw.up_weight_name = model->name_storage_.back();
        model->name_storage_.push_back(names.down_weight);
        lw.down_weight_name = model->name_storage_.back();

        // Norm weights — resolved as TensorView (required for every family).
        lw.attn_norm = find_weight(weights, names.attn_norm);
        lw.mlp_norm = find_weight(weights, names.mlp_norm);
        if (!lw.attn_norm.valid() || !lw.mlp_norm.valid()) {
            return Status::Error(ErrorCode::kNotFound,
                                 "missing norm weight for layer " + std::to_string(l));
        }
        if (auto s = require_elementwise_weight(lw.attn_norm, names.attn_norm); !s.ok()) {
            return s;
        }
        if (auto s = require_elementwise_weight(lw.mlp_norm, names.mlp_norm); !s.ok()) {
            return s;
        }

        // Matmul weights: existence + supported dtype (fail fast at load —
        // previously missing ones only errored deep inside the first MatMul).
        for (const std::string_view wname : {lw.q_weight_name,
                                             lw.k_weight_name,
                                             lw.v_weight_name,
                                             lw.o_weight_name,
                                             lw.gate_weight_name,
                                             lw.up_weight_name,
                                             lw.down_weight_name}) {
            if (auto s = require_matmul_weight(weights, wname); !s.ok()) {
                return s;
            }
        }

        // Optional family tensors; each is mandatory when its feature flag
        // is set so a malformed file fails fast instead of silently
        // generating wrong logits.
        if (config.qkv_bias) {
            lw.q_bias = find_weight(weights, names.q_bias);
            lw.k_bias = find_weight(weights, names.k_bias);
            lw.v_bias = find_weight(weights, names.v_bias);
            if (!lw.q_bias.valid() || !lw.k_bias.valid() || !lw.v_bias.valid()) {
                return Status::Error(ErrorCode::kNotFound,
                                     "missing qkv bias weight for layer " + std::to_string(l));
            }
            if (auto s = require_elementwise_weight(lw.q_bias, names.q_bias); !s.ok()) {
                return s;
            }
            if (auto s = require_elementwise_weight(lw.k_bias, names.k_bias); !s.ok()) {
                return s;
            }
            if (auto s = require_elementwise_weight(lw.v_bias, names.v_bias); !s.ok()) {
                return s;
            }
        }
        if (config.qk_norm) {
            lw.q_norm = find_weight(weights, names.q_norm);
            lw.k_norm = find_weight(weights, names.k_norm);
            if (!lw.q_norm.valid() || !lw.k_norm.valid()) {
                return Status::Error(ErrorCode::kNotFound,
                                     "missing qk norm weight for layer " + std::to_string(l));
            }
            if (auto s = require_elementwise_weight(lw.q_norm, names.q_norm); !s.ok()) {
                return s;
            }
            if (auto s = require_elementwise_weight(lw.k_norm, names.k_norm); !s.ok()) {
                return s;
            }
        }

        model->layers_.emplace_back(l, lw);
    }

    return model;
}

Status DenseDecoderModel::Forward(TensorView hidden,
                                  int64_t position,
                                  KVCache& cache,
                                  Backend& backend,
                                  ScratchArena& scratch) const {
    for (const auto& layer : layers_) {
        if (auto s = layer.Forward(hidden, position, cache, backend, scratch, config_); !s.ok()) {
            return s;
        }
    }
    // All layers have appended; advance the cache position
    cache.Advance();
    return {};
}

Status DenseDecoderModel::Prefill(TensorView hidden,
                                  int64_t start_pos,
                                  KVCache& cache,
                                  Backend& backend,
                                  ScratchArena& scratch) const {
    const int32_t n = static_cast<int32_t>(hidden.shape().dim(0));
    if (n <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "Prefill: empty batch");
    }
    // The residual stream must survive all layers; it lives in the
    // caller-owned `hidden` buffer. Per-layer activations are intra-layer
    // only, so the scratch arena can be reset before each layer.
    for (const auto& layer : layers_) {
        scratch.Reset();
        if (auto s = layer.ForwardBatch(hidden, start_pos, cache, backend, scratch, config_);
            !s.ok()) {
            return s;
        }
    }
    // All layers have appended the whole batch; advance the cache length.
    cache.Advance(n);
    return {};
}

Status DenseDecoderModel::ComputeLogits(TensorView hidden,
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

std::vector<std::string> DenseDecoderModel::weight_names() const {
    std::vector<std::string> names;
    // Output weight
    names.emplace_back(output_weight_name_);
    // Output norm
    names.emplace_back("output_norm.weight");
    // Per-layer
    for (int32_t l = 0; l < config_.num_layers; ++l) {
        const LayerWeightNames w = make_layer_weight_names(l, config_.qkv_bias, config_.qk_norm);
        names.push_back(w.q_weight);
        names.push_back(w.k_weight);
        names.push_back(w.v_weight);
        names.push_back(w.o_weight);
        names.push_back(w.attn_norm);
        names.push_back(w.gate_weight);
        names.push_back(w.up_weight);
        names.push_back(w.down_weight);
        names.push_back(w.mlp_norm);
        if (config_.qkv_bias) {
            names.push_back(w.q_bias);
            names.push_back(w.k_bias);
            names.push_back(w.v_bias);
        }
        if (config_.qk_norm) {
            names.push_back(w.q_norm);
            names.push_back(w.k_norm);
        }
    }
    // Token embedding (always needed, even if tied)
    names.emplace_back("token_embd.weight");
    return names;
}

} // namespace pl::mllm
