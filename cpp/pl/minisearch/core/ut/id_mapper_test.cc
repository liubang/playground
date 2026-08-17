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
// Created: 2026/08/17

#include <gtest/gtest.h>
#include <optional>

#include "cpp/pl/minisearch/core/id_mapper.h"

namespace pmc = pl::minisearch::core;

TEST(DocIdMapperTest, AssignNewIdHasNoTombstone) {
    pmc::DocIdMapper mapper;
    std::optional<int64_t> tombstoned;
    const int64_t first = mapper.Assign("doc1", &tombstoned);
    EXPECT_GE(first, 0);
    EXPECT_EQ(tombstoned, std::nullopt);
    EXPECT_EQ(mapper.Lookup("doc1"), first);
    EXPECT_EQ(mapper.ActiveSize(), 1u);
    EXPECT_TRUE(mapper.tombstones().empty());
}

TEST(DocIdMapperTest, ReassignMovesPreviousDocidToTombstones) {
    pmc::DocIdMapper mapper;
    std::optional<int64_t> tombstoned;
    const int64_t first = mapper.Assign("doc1", &tombstoned);
    const int64_t second = mapper.Assign("doc1", &tombstoned);
    EXPECT_NE(first, second);
    EXPECT_EQ(tombstoned, first);
    EXPECT_EQ(mapper.Lookup("doc1"), second);
    EXPECT_EQ(mapper.ActiveSize(), 1u);
    EXPECT_EQ(mapper.tombstones().size(), 1u);
}

TEST(DocIdMapperTest, RemoveTombstonesAndDropsLookup) {
    pmc::DocIdMapper mapper;
    std::optional<int64_t> tombstoned;
    const int64_t first = mapper.Assign("doc1", &tombstoned);
    EXPECT_TRUE(mapper.Remove("doc1"));
    EXPECT_EQ(mapper.Lookup("doc1"), -1);
    EXPECT_EQ(mapper.ActiveSize(), 0u);
    EXPECT_EQ(mapper.tombstones().size(), 1u);
    EXPECT_TRUE(mapper.tombstones().count(first) > 0);
    EXPECT_FALSE(mapper.Remove("doc1"));
}
