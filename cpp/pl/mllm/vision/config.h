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

#include <cstdint>
#include <string>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm::vision {

// Hyperparameters of a vision tower (ViT encoder + projector), parsed from
// the mmproj GGUF `clip.*` metadata. The layout mirrors ModelConfig on the
// text side: plain data + Validate(), family differences stay in the
// tower implementation.
struct VisionConfig {
    // Projector family (GGUF `clip.projector_type`), e.g. "paddleocr".
    std::string projector_type;

    int32_t hidden_size = 0;       // vision encoder width (e.g. 1152)
    int32_t intermediate_size = 0; // vision FFN width (e.g. 4304)
    int32_t num_layers = 0;
    int32_t num_heads = 0;
    int32_t patch_size = 14;
    int32_t spatial_merge_size = 2;
    // Projector output width == LM hidden_size. May be 0 before weight
    // resolution: towers infer it from the projector's final linear.
    int32_t output_dim = 0;

    float layer_norm_eps = 1e-6f;
    // Projector input norm eps (PaddleOCR-VL uses 1e-5 there).
    float projector_norm_eps = 1e-5f;
    bool gelu_tanh = true;

    // Image preprocessing bounds (GGUF clip.vision.min/max_pixels).
    int64_t min_pixels = 0;
    int64_t max_pixels = 0;
    float image_mean[3] = {0.5f, 0.5f, 0.5f};
    float image_std[3] = {0.5f, 0.5f, 0.5f};

    // 2D rotary (vision MRoPE) base frequency inside the encoder.
    float rope_freq_base = 10000.0f;

    [[nodiscard]] int32_t head_dim() const { return num_heads > 0 ? hidden_size / num_heads : 0; }

    [[nodiscard]] Status Validate() const {
        // output_dim may legitimately be 0 here: towers resolve it from the
        // projector weights during Create (see above).
        if (hidden_size <= 0 || intermediate_size <= 0 || num_layers <= 0 || num_heads <= 0 ||
            patch_size <= 0 || spatial_merge_size <= 0 || output_dim < 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "vision config: non-positive dimension");
        }
        if (hidden_size % num_heads != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "vision config: hidden_size not divisible by heads");
        }
        if (head_dim() % 2 != 0) {
            return Status::Error(ErrorCode::kInvalidFormat,
                                 "vision config: head_dim must be even for RoPE");
        }
        return {};
    }
};

} // namespace pl::mllm::vision
