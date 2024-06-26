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
#include "cpp/pl/sst/block.h"
#include "cpp/pl/sst/block_builder.h"
#include "cpp/pl/sst/encoding.h"

#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

namespace pl {

class BlockBuilderTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(BlockBuilderTest, test) {
    auto options = std::make_unique<BuildOptions>();
    options->block_restart_interval = 16;

    pl::BlockBuilder block_builder(std::move(options));
    constexpr int COUNT = 10001;

    EXPECT_TRUE(block_builder.empty());

    std::vector<std::string> keys;
    std::unordered_map<std::string, std::string> kvs;
    const std::string key_prefix = "test_key_";
    for (int i = 0; i < COUNT; ++i) {
        std::string key = key_prefix + std::to_string(i);
        keys.push_back(key);
    }

    std::sort(keys.begin(), keys.end());

    for (int i = 0; i < COUNT; ++i) {
        auto val = pl::random_string(64);
        block_builder.add(keys[i], val);
        kvs[keys[i]] = val;
    }

    EXPECT_TRUE(!block_builder.empty());

    auto block = block_builder.finish();

    pl::BlockContents block_content = {
        block,
        false,
        false,
    };

    auto b = std::make_shared<Block>(block_content);
    auto itr = b->iterator(nullptr);

    itr->first();
    while (itr->valid()) {
        auto k = itr->key();
        auto v = itr->val();

        std::string key(k.data(), k.size());
        std::string val(v.data(), v.size());

        EXPECT_TRUE(kvs.contains(key));
        EXPECT_TRUE(kvs[key] == val);

        itr->next();
    }

    // try to parse
    const char* data = block.data();
    std::size_t size = block.size();
    auto restart_count = pl::decodeInt<uint32_t>(&data[size - 4]);
    EXPECT_EQ(restart_count, (COUNT / 16) + (COUNT % 16 == 0 ? 0 : 1));

    // parse all restarts
    std::vector<uint32_t> restarts(restart_count + 1);
    for (int i = 0; i < restart_count; ++i) {
        uint32_t offset = 4 * (i + 2);
        auto restart = pl::decodeInt<uint32_t>(&data[size - offset]);
        restarts[i] = restart;
    }
    restarts[restart_count] = size - (4 * (restart_count + 1));
    std::string pre_key;

    // parse keys and values
    for (int i = 0; i < restart_count; ++i) {
        // 计算每一个restart的起始位置和结束位置
        uint32_t start = restarts[i];
        uint32_t end = restarts[i + 1];
        // 解析每一个restart中的key和value
        while (start < end) {
            assert((start + 12) < end);
            auto shared = pl::decodeInt<uint32_t>(data + start);
            auto non_shared = pl::decodeInt<uint32_t>(data + start + 4);
            auto value_size = pl::decodeInt<uint32_t>(data + start + 8);
            assert(start + 12 + non_shared + value_size <= end);

            assert(pre_key.size() >= shared);
            std::string key(pre_key.data(), shared);
            key.append(data + start + 12, non_shared);
            pre_key = key;
            std::string value(data + start + 12 + non_shared, value_size);

            EXPECT_TRUE(kvs.contains(key));
            EXPECT_TRUE(kvs[key] == value);

            start += 12 + non_shared + value_size;
        }
    }
}

} // namespace pl
