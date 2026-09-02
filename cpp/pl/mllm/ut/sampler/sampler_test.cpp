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

// Regression tests for two historical bugs:
//  1. The repetition penalty was silently inert because the engine mutated a
//     copy of SamplerParams after the Sampler had already snapshotted it.
//  2. Stochastic sampling reseeded the PRNG on every Sample call, producing
//     the identical uniform draw for every token.

#include <gtest/gtest.h>
#include <vector>

#include "cpp/pl/mllm/sampler/sampler.h"

using namespace pl::mllm;

TEST(SamplerTest, GreedyPicksArgmax) {
    SamplerParams sp; // temperature 0 = greedy
    Sampler sampler(sp);
    const std::vector<float> logits = {0.1f, 2.5f, -1.0f, 0.5f};
    EXPECT_EQ(sampler.Sample(logits), 1);
}

TEST(SamplerTest, RepeatPenaltySuppressesRecentToken) {
    SamplerParams sp; // greedy + penalty on
    sp.repeat_penalty = 2.0f;
    Sampler sampler(sp);

    // Token 1 has the top logit; once it appears in the penalty window its
    // (positive) logit is divided by the penalty and token 3 must win.
    const std::vector<float> logits = {0.1f, 2.0f, -1.0f, 1.5f};
    EXPECT_EQ(sampler.Sample(logits), 1);

    const std::vector<int32_t> history = {1};
    sampler.set_penalty_tokens(history);
    EXPECT_EQ(sampler.Sample(logits), 3);

    // Clearing the window restores the argmax.
    sampler.set_penalty_tokens({});
    EXPECT_EQ(sampler.Sample(logits), 1);
}

TEST(SamplerTest, RepeatPenaltyHandlesNegativeLogits) {
    // llama.cpp semantics: negative logits are *multiplied* by the penalty
    // (pushed further down).
    SamplerParams sp;
    sp.repeat_penalty = 2.0f;
    Sampler sampler(sp);

    // Token 0 has the top logit (-1.0 vs -1.6). Penalized: -1.0 * 2 = -2.0,
    // which drops below token 1.
    const std::vector<float> logits = {-1.0f, -1.6f};
    EXPECT_EQ(sampler.Sample(logits), 0);
    const std::vector<int32_t> hist = {0};
    sampler.set_penalty_tokens(hist);
    EXPECT_EQ(sampler.Sample(logits), 1);
}

TEST(SamplerTest, StochasticDrawsVaryAcrossSteps) {
    SamplerParams sp;
    sp.temperature = 1.0f;
    sp.seed = 42;
    Sampler sampler(sp);

    // Wipe any signal: flat logits over many tokens. With a fresh PRNG per
    // step (the historical bug), the same token would be drawn every time;
    // persistent PRNG state must produce varying picks.
    const std::vector<float> logits(1024, 0.0f);
    std::vector<int32_t> picks;
    for (int i = 0; i < 64; ++i) {
        picks.push_back(sampler.Sample(logits));
    }
    int distinct = 0;
    for (size_t i = 1; i < picks.size(); ++i) {
        if (picks[i] != picks[0]) {
            ++distinct;
        }
    }
    EXPECT_GT(distinct, 0);
}

TEST(SamplerTest, DeterministicGivenSameSeed) {
    const std::vector<float> logits(256, 0.0f);

    SamplerParams sp;
    sp.temperature = 1.0f;
    sp.seed = 7;

    Sampler a(sp);
    Sampler b(sp);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(a.Sample(logits), b.Sample(logits));
    }
}
