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

#include "cpp/pl/mllm/sampler/sampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pl::mllm {

namespace {

uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

} // namespace

Sampler::Rng::Rng(uint64_t seed) {
    // splitmix64 seeding
    uint64_t z = seed;
    const auto next = [&] {
        z += 0x9E3779B97F4A7C15ull;
        uint64_t x = z;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x = x ^ (x >> 31);
        return x;
    };
    s[0] = next();
    s[1] = next();
    s[2] = next();
    s[3] = next();
}

uint32_t Sampler::Rng::operator()() {
    const uint32_t result = static_cast<uint32_t>(rotl(s[1] * 5, 7) * 9);
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

Sampler::Sampler(SamplerParams params) : params_(params), rng_(params.seed) {}

void Sampler::set_penalty_tokens(std::span<const int32_t> tokens) {
    penalty_tokens_ = tokens;
}

int32_t Sampler::Sample(std::span<const float> logits) const {
    std::vector<LogitProbs> tmp;
    return Sample(logits, tmp);
}

int32_t Sampler::Sample(std::span<const float> logits,
                        std::vector<LogitProbs>& out_candidates) const {
    const int32_t n = static_cast<int32_t>(logits.size());
    if (n == 0) {
        return -1;
    }

    // Apply repeat penalty to specified tokens. The logits are only copied
    // when the penalty is active — the common greedy path samples straight
    // from the input span (no per-token vocab-sized alloc + memcpy).
    std::span<const float> eff = logits;
    std::vector<float> adjusted;
    if (params_.repeat_penalty != 1.0f && !penalty_tokens_.empty()) {
        adjusted.assign(logits.begin(), logits.end());
        for (const int32_t t : penalty_tokens_) {
            if (t >= 0 && t < n) {
                // llama.cpp style: divide if positive, multiply if negative
                const float lp = adjusted[static_cast<size_t>(t)];
                adjusted[static_cast<size_t>(t)] =
                    (lp > 0.0f) ? lp / params_.repeat_penalty : lp * params_.repeat_penalty;
            }
        }
        eff = adjusted;
    }

    // Greedy path.
    if (params_.temperature <= 0.0f) {
        int32_t best = 0;
        float bestv = eff[0];
        for (int32_t i = 1; i < n; ++i) {
            if (eff[static_cast<size_t>(i)] > bestv) {
                bestv = eff[static_cast<size_t>(i)];
                best = i;
            }
        }
        out_candidates.clear();
        out_candidates.push_back({best, bestv});
        return best;
    }

    // Stochastic path.
    std::vector<LogitProbs> cands;
    cands.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        cands.push_back({i, eff[static_cast<size_t>(i)] / params_.temperature});
    }

    // top-k
    if (params_.top_k > 0 && params_.top_k < n) {
        std::nth_element(
            cands.begin(),
            cands.begin() + (params_.top_k - 1),
            cands.end(),
            [](const LogitProbs& a, const LogitProbs& b) { return a.logit > b.logit; });
        cands.resize(static_cast<size_t>(params_.top_k));
    }

    // softmax
    float maxl = cands.front().logit;
    for (const auto& c : cands)
        maxl = std::max(maxl, c.logit);
    float z = 0.0f;
    for (auto& c : cands) {
        c.logit = std::exp(c.logit - maxl);
        z += c.logit;
    }

    // top-p
    if (params_.top_p < 1.0f) {
        std::sort(cands.begin(), cands.end(), [](const LogitProbs& a, const LogitProbs& b) {
            return a.logit > b.logit;
        });
        float cum = 0.0f;
        size_t cut = cands.size();
        for (size_t i = 0; i < cands.size(); ++i) {
            cum += cands[i].logit / z;
            if (cum >= params_.top_p) {
                cut = i + 1;
                break;
            }
        }
        cands.resize(cut);
        z = 0.0f;
        for (const auto& c : cands)
            z += c.logit;
    }

    float r = static_cast<float>(rng_() >> 11) / static_cast<float>(1u << 21);
    float cum = 0.0f;
    int32_t chosen = cands.back().token;
    for (const auto& c : cands) {
        cum += c.logit / z;
        if (r <= cum) {
            chosen = c.token;
            break;
        }
    }
    out_candidates = std::move(cands);
    return chosen;
}

} // namespace pl::mllm
