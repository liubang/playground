// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/sst/filter_policy.h"
#include <gtest/gtest.h>

namespace pl {

class FilterPolicyTest : public ::testing::Test,
                         public ::testing::WithParamInterface<std::tuple<std::string, uint32_t>> {
public:
    void SetUp() override {
        filter_policy_name_ = std::get<0>(GetParam());
        bits_per_key_ = std::get<1>(GetParam());
    }

    void TearDown() override {}

protected:
    static FilterBuilderPtr create(uint32_t bits_per_key, const std::string& name) {
        if (name == StandardBloomFilterBuilder::kClassName()) {
            return std::make_unique<StandardBloomFilterBuilder>(bits_per_key);
        }
        if (name == BlockedBloomFilterBuilder::kClassName()) {
            return std::make_unique<BlockedBloomFilterBuilder>(bits_per_key);
        }
        return nullptr;
    }

protected:
    std::string filter_policy_name_;
    uint32_t bits_per_key_;
};

TEST_P(FilterPolicyTest, BloomFilterDedup) {
    auto builder = create(bits_per_key_, filter_policy_name_);
    EXPECT_EQ(0U, builder->estimate_hashes_added());
    builder->add_key("abc");
    EXPECT_EQ(1U, builder->estimate_hashes_added());
    builder->add_key_and_alt("abc1", "abc");
    EXPECT_EQ(2U, builder->estimate_hashes_added());
    builder->add_key_and_alt("bcd", "bcd");
    EXPECT_EQ(3U, builder->estimate_hashes_added());
    builder->add_key_and_alt("cde-1", "cde");
    EXPECT_EQ(5U, builder->estimate_hashes_added());
    builder->add_key_and_alt("cde", "cde");
    EXPECT_EQ(5U, builder->estimate_hashes_added());
    builder->add_key_and_alt("cde1", "cde");
    EXPECT_EQ(6U, builder->estimate_hashes_added());
    builder->add_key_and_alt("def-1", "def");
    EXPECT_EQ(8U, builder->estimate_hashes_added());
    builder->add_key_and_alt("def", "def");
    EXPECT_EQ(8U, builder->estimate_hashes_added());
    builder->add_key("def$$"); // Like not in extractor domain
    EXPECT_EQ(9U, builder->estimate_hashes_added());
    builder->add_key("def$$");
    EXPECT_EQ(9U, builder->estimate_hashes_added());
    builder->add_key_and_alt("efg42", "efg");
    EXPECT_EQ(11U, builder->estimate_hashes_added());
    builder->add_key_and_alt("efg", "efg"); // Like extra "alt" on a partition
    EXPECT_EQ(11U, builder->estimate_hashes_added());
}

INSTANTIATE_TEST_CASE_P(
    FormatDef,
    FilterPolicyTest,
    ::testing::Values(std::make_tuple(StandardBloomFilterBuilder::kClassName(), 10),
                      std::make_tuple(BlockedBloomFilterBuilder::kClassName(), 10)));

} // namespace pl
