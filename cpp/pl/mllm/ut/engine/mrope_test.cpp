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

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

#include "cpp/pl/mllm/engine/mrope.h"

namespace pl::mllm {
namespace {

// ---------------------------------------------------------------------------
// BuildMRopePlan
// ---------------------------------------------------------------------------

TEST(MRopePlanTest, EmptySegments) {
    const MRopePlan plan = BuildMRopePlan({});
    EXPECT_TRUE(plan.positions.empty());
    EXPECT_EQ(plan.delta, 0);
}

TEST(MRopePlanTest, TextOnlyIsClassicPositions) {
    const PromptSegment segs[] = {PromptSegment::Text(3)};
    const MRopePlan plan = BuildMRopePlan(segs);
    ASSERT_EQ(plan.positions.size(), 3u);
    for (int64_t i = 0; i < 3; ++i) {
        EXPECT_EQ(plan.positions[static_cast<size_t>(i)], (std::array<int64_t, 3>{i, i, i}));
    }
    // Text-only prompts must collapse to the classic rope convention:
    // decode rope position == sequence index.
    EXPECT_EQ(plan.delta, 0);
}

TEST(MRopePlanTest, SingleImageGridRowMajor) {
    const PromptSegment segs[] = {PromptSegment::Image(2, 3)};
    const MRopePlan plan = BuildMRopePlan(segs);
    ASSERT_EQ(plan.positions.size(), 6u);
    // Shared temporal coordinate 0, row-major (h, w) enumeration.
    const std::array<int64_t, 3> expected[] = {
        {0, 0, 0},
        {0, 0, 1},
        {0, 0, 2},
        {0, 1, 0},
        {0, 1, 1},
        {0, 1, 2},
    };
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(plan.positions[i], expected[i]) << "index " << i;
    }
    // Next free coordinate is st = max(2, 3) = 3: delta = 3 - 6 = -3.
    EXPECT_EQ(plan.delta, -3);
}

TEST(MRopePlanTest, MixedTextImageText) {
    const PromptSegment segs[] = {
        PromptSegment::Text(2),
        PromptSegment::Image(1, 2),
        PromptSegment::Text(1),
    };
    const MRopePlan plan = BuildMRopePlan(segs);
    ASSERT_EQ(plan.positions.size(), 5u);
    EXPECT_EQ(plan.positions[0], (std::array<int64_t, 3>{0, 0, 0}));
    EXPECT_EQ(plan.positions[1], (std::array<int64_t, 3>{1, 1, 1}));
    // Image starts at st = 2: temporal shared, (y, x) enumerates the 1x2 grid.
    EXPECT_EQ(plan.positions[2], (std::array<int64_t, 3>{2, 2, 2}));
    EXPECT_EQ(plan.positions[3], (std::array<int64_t, 3>{2, 2, 3}));
    // After the image st advances by max(1, 2): 2 -> 4, so text resumes at 4.
    EXPECT_EQ(plan.positions[4], (std::array<int64_t, 3>{4, 4, 4}));
    // st = 5 == number of positions -> delta 0.
    EXPECT_EQ(plan.delta, 0);
}

TEST(MRopePlanTest, TallImageUsesHeightForAdvance) {
    // 3x1 image: st advances by max(3, 1) = 3, so the trailing text starts at
    // 3 (not at the token count 3 + 0 — same here, but a non-square case).
    const PromptSegment segs[] = {PromptSegment::Image(3, 1), PromptSegment::Text(2)};
    const MRopePlan plan = BuildMRopePlan(segs);
    ASSERT_EQ(plan.positions.size(), 5u);
    EXPECT_EQ(plan.positions[2], (std::array<int64_t, 3>{0, 2, 0})); // last cell (y=2,x=0)
    EXPECT_EQ(plan.positions[3], (std::array<int64_t, 3>{3, 3, 3}));
    EXPECT_EQ(plan.positions[4], (std::array<int64_t, 3>{4, 4, 4}));
    // st = 5 == positions.size() -> delta 0.
    EXPECT_EQ(plan.delta, 0);
}

TEST(MRopePlanTest, WideThenTallTextDelta) {
    // 4x1 image followed by 1x4 image: verifies per-segment accumulation.
    const PromptSegment segs[] = {PromptSegment::Image(1, 4), PromptSegment::Image(4, 1)};
    const MRopePlan plan = BuildMRopePlan(segs);
    ASSERT_EQ(plan.positions.size(), 8u);
    // First image: st = max(1, 4) = 4 after 4 tokens.
    // Second image starts at st = 4 and also advances by max(4, 1) = 4.
    EXPECT_EQ(plan.positions[4], (std::array<int64_t, 3>{4, 4, 4}));
    EXPECT_EQ(plan.positions[7], (std::array<int64_t, 3>{4, 7, 4}));
    EXPECT_EQ(plan.delta, 8 - 8);
}

// ---------------------------------------------------------------------------
// BuildMRopeTables
// ---------------------------------------------------------------------------

TEST(MRopeTablesTest, SingleRowMatchesFormula) {
    const int32_t head_dim = 8;
    const std::array<int32_t, 3> sections = {2, 1, 1}; // sums to head_dim/2 = 4
    const float base = 10000.0f;
    const std::array<std::array<int64_t, 3>, 1> positions = {{{5, 1, 2}}};

    std::vector<float> cos, sin;
    BuildMRopeTables(positions, head_dim, sections, base, cos, sin);
    ASSERT_EQ(cos.size(), static_cast<size_t>(head_dim));
    ASSERT_EQ(sin.size(), static_cast<size_t>(head_dim));

    for (int32_t i = 0; i < head_dim / 2; ++i) {
        const int64_t coord = i < 2 ? 5 : (i == 2 ? 1 : 2);
        const float inv =
            std::pow(base, -2.0f * static_cast<float>(i) / static_cast<float>(head_dim));
        const float angle = static_cast<float>(coord) * inv;
        EXPECT_NEAR(cos[static_cast<size_t>(i)], std::cos(angle), 1e-6f) << "t" << i;
        EXPECT_NEAR(sin[static_cast<size_t>(i)], std::sin(angle), 1e-6f) << "t" << i;
        // Neox layout: the second half duplicates the first.
        EXPECT_EQ(cos[static_cast<size_t>(i + head_dim / 2)], cos[static_cast<size_t>(i)]);
        EXPECT_EQ(sin[static_cast<size_t>(i + head_dim / 2)], sin[static_cast<size_t>(i)]);
    }
}

TEST(MRopeTablesTest, SectionBoundariesPickTripleComponents) {
    // head_dim = 4, sections {2, 1, 1}: pair 0 -> t, pair 1 -> h,
    // w never used (sections sum must equal head_dim/2 = 2).
    const int32_t head_dim = 4;
    const std::array<int32_t, 3> sections = {1, 0, 1};
    const float base = 1000.0f;
    const std::array<std::array<int64_t, 3>, 1> positions = {{{3, 7, 11}}};

    std::vector<float> cos, sin;
    BuildMRopeTables(positions, head_dim, sections, base, cos, sin);
    ASSERT_EQ(cos.size(), 4u);
    // pair 0: t = 3 with freq base^(0) = 1
    EXPECT_NEAR(cos[0], std::cos(3.0f), 1e-6f);
    // pair 1: sections {1, 0, 1} -> b1 = 1 + 0 = 1, so i=1 falls in the w
    // section: w = 11 with freq base^(-2*1/4) = base^-0.5
    const float angle = 11.0f * std::pow(1000.0f, -0.5f);
    EXPECT_NEAR(cos[1], std::cos(angle), 1e-6f);
    EXPECT_NEAR(sin[1], std::sin(angle), 1e-6f);
}

TEST(MRopeTablesTest, EachRowUsesItsOwnPosition) {
    const int32_t head_dim = 4;
    const std::array<int32_t, 3> sections = {1, 1, 0};
    const float base = 100.0f;
    const std::array<std::array<int64_t, 3>, 2> positions = {{
        {{0, 0, 0}},
        {{2, 4, 0}},
    }};
    std::vector<float> cos, sin;
    BuildMRopeTables(positions, head_dim, sections, base, cos, sin);
    ASSERT_EQ(cos.size(), 8u);
    // Row 0: all coordinates 0 -> cos 1, sin 0.
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(cos[i], 1.0f);
        EXPECT_FLOAT_EQ(sin[i], 0.0f);
    }
    // Row 1: pair 0 uses t = 2 (base^0 = 1), pair 1 uses h = 4 (base^-0.5 = 0.1).
    EXPECT_NEAR(cos[4], std::cos(2.0f), 1e-6f);
    EXPECT_NEAR(cos[5], std::cos(0.4f), 1e-6f);
}

TEST(MRopeTablesTest, EqualTripleMatchesClassicRope) {
    // When (t, h, w) are equal, every section picks the same coordinate and
    // the table degenerates to the classic 1D rope table.
    const int32_t head_dim = 8;
    const std::array<int32_t, 3> sections = {2, 1, 1};
    const float base = 10000.0f;
    const int64_t p = 7;
    const std::array<std::array<int64_t, 3>, 1> positions = {{{p, p, p}}};
    std::vector<float> cos, sin;
    BuildMRopeTables(positions, head_dim, sections, base, cos, sin);
    for (int32_t i = 0; i < head_dim / 2; ++i) {
        const float angle =
            static_cast<float>(p) *
            std::pow(base, -2.0f * static_cast<float>(i) / static_cast<float>(head_dim));
        EXPECT_NEAR(cos[static_cast<size_t>(i)], std::cos(angle), 1e-6f);
        EXPECT_NEAR(sin[static_cast<size_t>(i)], std::sin(angle), 1e-6f);
    }
}

} // namespace
} // namespace pl::mllm
