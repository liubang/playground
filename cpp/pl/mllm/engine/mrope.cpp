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

#include "cpp/pl/mllm/engine/mrope.h"

#include <algorithm>
#include <cmath>

namespace pl::mllm {

MRopePlan BuildMRopePlan(std::span<const PromptSegment> segments) {
    MRopePlan plan;
    int64_t st = 0; // next free coordinate (max used + 1)
    for (const auto& seg : segments) {
        if (seg.text_tokens > 0) {
            plan.positions.reserve(plan.positions.size() + static_cast<size_t>(seg.text_tokens));
            for (int32_t i = 0; i < seg.text_tokens; ++i) {
                plan.positions.push_back({st + i, st + i, st + i});
            }
            st += seg.text_tokens;
        } else if (seg.image_grid_h > 0 && seg.image_grid_w > 0) {
            const size_t cells =
                static_cast<size_t>(seg.image_grid_h) * static_cast<size_t>(seg.image_grid_w);
            plan.positions.reserve(plan.positions.size() + cells);
            for (int32_t y = 0; y < seg.image_grid_h; ++y) {
                for (int32_t x = 0; x < seg.image_grid_w; ++x) {
                    // Single-frame image: the temporal coordinate is shared.
                    plan.positions.push_back({st, st + y, st + x});
                }
            }
            st += std::max(seg.image_grid_h, seg.image_grid_w);
        }
    }
    plan.delta = st - static_cast<int64_t>(plan.positions.size());
    return plan;
}

void BuildMRopeTables(std::span<const std::array<int64_t, 3>> positions,
                      int32_t head_dim,
                      const std::array<int32_t, 3>& sections,
                      float freq_base,
                      std::vector<float>& cos_out,
                      std::vector<float>& sin_out) {
    const int32_t half = head_dim / 2;
    const int32_t b0 = sections[0];
    const int32_t b1 = sections[0] + sections[1];
    cos_out.resize(positions.size() * static_cast<size_t>(head_dim));
    sin_out.resize(positions.size() * static_cast<size_t>(head_dim));
    for (size_t r = 0; r < positions.size(); ++r) {
        const auto [t, h, w] = positions[r];
        float* crow = cos_out.data() + r * static_cast<size_t>(head_dim);
        float* srow = sin_out.data() + r * static_cast<size_t>(head_dim);
        for (int32_t i = 0; i < half; ++i) {
            const int64_t coord = i < b0 ? t : (i < b1 ? h : w);
            const float inv =
                std::pow(freq_base, -2.0f * static_cast<float>(i) / static_cast<float>(head_dim));
            const float angle = static_cast<float>(coord) * inv;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            crow[i] = c;
            crow[i + half] = c;
            srow[i] = s;
            srow[i + half] = s;
        }
    }
}

} // namespace pl::mllm
