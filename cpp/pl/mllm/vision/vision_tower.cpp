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

#include "cpp/pl/mllm/vision/vision_tower.h"

#include <span>

#include "cpp/pl/mllm/loader/gguf.h"
#include "cpp/pl/mllm/vision/paddleocr_tower.h"

namespace pl::mllm::vision {

namespace {

// Required scalar metadata; missing keys abort with the key name.
Status read_i32(const GGUFFile& f, std::string_view key, int32_t& out) {
    auto v = f.i32_meta(key);
    if (!v.ok()) {
        return v.status();
    }
    out = v.value();
    return {};
}

// Optional scalar metadata with a default.
void read_i32_or(const GGUFFile& f, std::string_view key, int32_t& out, int32_t fallback) {
    if (auto v = f.i32_meta(key); v.ok()) {
        out = v.value();
    } else {
        out = fallback;
    }
}

void read_f32_or(const GGUFFile& f, std::string_view key, float& out, float fallback) {
    if (auto v = f.f32_meta(key); v.ok()) {
        out = v.value();
    } else {
        out = fallback;
    }
}

void read_vec3_or(const GGUFFile& f, std::string_view key, float out[3], const float fallback[3]) {
    auto v = f.f32_array_meta(key);
    if (!v.ok() || v.value().size() != 3) {
        out[0] = fallback[0];
        out[1] = fallback[1];
        out[2] = fallback[2];
        return;
    }
    out[0] = v.value()[0];
    out[1] = v.value()[1];
    out[2] = v.value()[2];
}

// Parse the `clip.*` metadata of an mmproj GGUF into a VisionConfig.
// Key names follow the llama.cpp mmproj converter conventions.
Result<VisionConfig> vision_config_from_gguf(const GGUFFile& mmproj) {
    auto type = mmproj.string_meta("clip.projector_type");
    if (!type.ok()) {
        return Status::Error(ErrorCode::kInvalidFormat, "mmproj: missing clip.projector_type");
    }

    VisionConfig cfg;
    cfg.projector_type = std::move(type).value();

    if (auto s = read_i32(mmproj, "clip.vision.embedding_length", cfg.hidden_size); !s.ok()) {
        return s;
    }
    if (auto s = read_i32(mmproj, "clip.vision.feed_forward_length", cfg.intermediate_size);
        !s.ok()) {
        return s;
    }
    if (auto s = read_i32(mmproj, "clip.vision.block_count", cfg.num_layers); !s.ok()) {
        return s;
    }
    if (auto s = read_i32(mmproj, "clip.vision.attention.head_count", cfg.num_heads); !s.ok()) {
        return s;
    }
    if (auto s = read_i32(mmproj, "clip.vision.patch_size", cfg.patch_size); !s.ok()) {
        return s;
    }

    read_i32_or(mmproj, "clip.vision.spatial_merge_size", cfg.spatial_merge_size, 2);
    // Projector output width; 0 = infer from the projector weights (the
    // tower resolves it from the final linear's output dimension).
    read_i32_or(mmproj, "clip.vision.projection_dim", cfg.output_dim, 0);
    read_f32_or(mmproj, "clip.vision.attention.layer_norm_epsilon", cfg.layer_norm_eps, 1e-6f);
    read_f32_or(mmproj, "clip.vision.rope_theta", cfg.rope_freq_base, 10000.0f);

    // Preprocessing pixel bounds. Real converters write `image_min_pixels` /
    // `image_max_pixels` (llama.cpp conventions); keep the shorter variants
    // as fallbacks for homegrown mmproj files.
    int32_t min_px = 0;
    int32_t max_px = 0;
    read_i32_or(mmproj, "clip.vision.image_min_pixels", min_px, 0);
    if (min_px == 0) {
        read_i32_or(mmproj, "clip.vision.min_pixels", min_px, 0);
    }
    read_i32_or(mmproj, "clip.vision.image_max_pixels", max_px, 0);
    if (max_px == 0) {
        read_i32_or(mmproj, "clip.vision.max_pixels", max_px, 0);
    }
    cfg.min_pixels = min_px;
    cfg.max_pixels = max_px;

    static constexpr float kHalf[3] = {0.5f, 0.5f, 0.5f};
    read_vec3_or(mmproj, "clip.vision.image_mean", cfg.image_mean, kHalf);
    read_vec3_or(mmproj, "clip.vision.image_std", cfg.image_std, kHalf);

    return cfg;
}

} // namespace

Result<std::unique_ptr<VisionTower>> CreateVisionTower(const GGUFFile& mmproj,
                                                       std::span<const WeightEntry> weights) {
    auto cfg = vision_config_from_gguf(mmproj);
    if (!cfg.ok()) {
        return cfg.status();
    }

    if (cfg.value().projector_type == "paddleocr") {
        auto tower = PaddleOcrTower::Create(std::move(cfg).value(), weights);
        if (!tower.ok()) {
            return tower.status();
        }
        return std::unique_ptr<VisionTower>(std::move(tower).value());
    }

    return Status::Error(ErrorCode::kUnsupported,
                         "unsupported mmproj projector_type: " + cfg.value().projector_type);
}

} // namespace pl::mllm::vision
