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
#include <span>
#include <vector>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm {

// Sampling parameters. temperature <= 0 means greedy.
struct SamplerParams {
    float temperature = 0.0f;
    int32_t top_k = 0;           // 0 = disabled
    float top_p = 1.0f;          // 1.0 = disabled
    float repeat_penalty = 1.0f; // 1.0 = disabled
    std::span<const int32_t> penalty_tokens{};
    uint64_t seed = 0;
};

struct LogitProbs {
    int32_t token = 0;
    float logit = 0.0f;
};

// Deterministic (seed-controlled) sampler. Greedy when temperature <= 0.
class Sampler {
public:
    explicit Sampler(SamplerParams params);

    // Returns the sampled token id and (optionally) the full candidate list.
    [[nodiscard]] int32_t Sample(std::span<const float> logits) const;
    [[nodiscard]] int32_t Sample(std::span<const float> logits,
                                 std::vector<LogitProbs>& out_candidates) const;

private:
    SamplerParams params_;
};

} // namespace pl::mllm
