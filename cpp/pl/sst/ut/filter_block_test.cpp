// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/filter_block_builder.h"
#include "cpp/pl/sst/filter_block_reader.h"
#include "cpp/pl/sst/filter_policy.h"

#include <memory>

#include <gtest/gtest.h>

namespace pl {

class FilterBlockTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(FilterBlockTest, build_and_read) {
    auto bloom_filter = std::make_shared<pl::BloomFilterPolicy>(10);
    pl::FilterBlockBuilder builder(bloom_filter);

    std::vector<std::string> keys;

    builder.startBlock(0);
    for (int i = 0; i < 1000; ++i) {
        auto str = pl::random_string(128);
        builder.addKey(str);
        keys.push_back(str);
    }

    auto filter = builder.finish();
    pl::FilterBlockReader reader(bloom_filter, filter);

    for (int i = 0; i < 1000; ++i) {
        auto ret = reader.keyMayMatch(0, keys[i]);
        EXPECT_TRUE(ret);
    }

    for (int i = 0; i < 1000; ++i) {
        auto str = pl::random_string(64);
        auto ret = reader.keyMayMatch(0, str);
        EXPECT_FALSE(ret);
    }
}

} // namespace pl
