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

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pl::mllm {

// One run of a multimodal prompt: either a text run (text_tokens > 0) or a
// merged image-token run (image_grid_h/image_grid_w > 0, tokens row-major
// over the merged patch grid).
struct PromptSegment {
    int32_t text_tokens = 0;
    int32_t image_grid_h = 0;
    int32_t image_grid_w = 0;

    [[nodiscard]] static PromptSegment Text(int32_t n) { return {.text_tokens = n}; }
    [[nodiscard]] static PromptSegment Image(int32_t grid_h, int32_t grid_w) {
        return {.image_grid_h = grid_h, .image_grid_w = grid_w};
    }
};

// Multimodal rotary (MRoPE) geometry of one whole prompt, following the
// Qwen2-VL conventions (matching HF `get_rope_index`):
//   * a text row gets the triple (p, p, p) with p counting up monotonically
//     across the prompt;
//   * an image run over the merged grid (gh, gw) gets (t, y, x) where t is
//     the frame coordinate shared by every token of the image and (y, x)
//     enumerates merged-grid cells row-major;
//   * every segment starts one past the maximum coordinate used so far;
//   * `delta` maps a cache sequence index to the rope position used during
//     decode: rope_position = delta + seq_index (0 for text-only prompts).
struct MRopePlan {
    std::vector<std::array<int64_t, 3>> positions; // one triple per prompt row
    int64_t delta = 0;
};

[[nodiscard]] MRopePlan BuildMRopePlan(std::span<const PromptSegment> segments);

// Builds [n, head_dim] cos/sin rotary tables (neox layout with duplicated
// halves; Backend::RopeApply consumes the first half of each row) from
// per-row position triples. Pair i of head_dim/2 rotates by
// coord(i) * freq_base^(-2i/head_dim), where coord(i) is the (t, h, w)
// component selected by `sections` (which must sum to head_dim/2).
void BuildMRopeTables(std::span<const std::array<int64_t, 3>> positions,
                      int32_t head_dim,
                      const std::array<int32_t, 3>& sections,
                      float freq_base,
                      std::vector<float>& cos_out,
                      std::vector<float>& sin_out);

} // namespace pl::mllm
