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
// Created: 2026/09/11

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/vision/config.h"
#include "cpp/pl/mllm/vision/vision_tower.h"

namespace pl::mllm::vision {

// PaddleOCR-VL vision tower (Baidu): a SigLIP-derived NaViT encoder —
// variable-resolution images, conv patchify, bilinearly interpolated
// learned position embeddings, ViT blocks with 2D rope (Qwen2-VL vision
// convention, neox pairing, h/w frequency halves) — followed by the
// "mlp_AR" projector: LayerNorm -> 2x2 spatial merge -> Linear -> GELU ->
// Linear into the LM hidden space.
//
// Reference semantics: HF transformers `models/paddleocr_vl` and
// llama.cpp `tools/mtmd/models/paddleocr.cpp`.
class PaddleOcrTower : public VisionTower {
public:
    // Build from the parsed vision config and the mmproj weight views.
    // The caller keeps the backing storage (mmap) alive.
    [[nodiscard]] static Result<std::unique_ptr<PaddleOcrTower>> Create(
        VisionConfig config, std::span<const WeightEntry> weights);

    [[nodiscard]] Result<VisionOutput> Encode(const media::Image& image,
                                              Backend& backend) const override;
    [[nodiscard]] Result<int32_t> TokenCount(int32_t width, int32_t height) const override;
    [[nodiscard]] const VisionConfig& config() const noexcept override { return config_; }
    [[nodiscard]] std::vector<std::string> weight_names() const override;

private:
    PaddleOcrTower() = default;

    struct LayerWeights {
        // LayerNorm affine params (weight + bias), resolved as views.
        TensorView ln1_w, ln1_b;
        TensorView ln2_w, ln2_b;
        // Attention projections — matmul weights by name, biases by view.
        std::string_view q_w, k_w, v_w, o_w;
        TensorView q_b, k_b, v_b, o_b;
        // MLP (SigLIP two-layer GELU MLP).
        std::string_view up_w, down_w;
        TensorView up_b, down_b;
    };

    VisionConfig config_;
    std::vector<LayerWeights> layers_;

    // Top-level weights.
    std::string_view patch_embd_w_; // [hidden, 3*p*p] flattened conv kernel
    TensorView patch_embd_b_;       // [hidden]
    TensorView pos_embd_;           // [ref_grid^2, hidden] learned table
    TensorView post_ln_w_;
    TensorView post_ln_b_;

    // Projector ("mlp_AR").
    TensorView proj_norm_w_;
    TensorView proj_norm_b_;
    std::string_view proj_fc1_w_; // [merge^2*hidden, merge^2*hidden]
    TensorView proj_fc1_b_;
    std::string_view proj_fc2_w_; // [output_dim, merge^2*hidden]
    TensorView proj_fc2_b_;

    // Per-layer weight name strings (kept alive for string_view references).
    std::vector<std::string> name_storage_;
};

} // namespace pl::mllm::vision
