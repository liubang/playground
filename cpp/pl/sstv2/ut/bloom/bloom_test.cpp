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
// Created: 2026/06/04 15:10

#include <cstddef>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/bloom/bloom_builder.h"
#include "cpp/pl/sstv2/bloom/bloom_reader.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::bloom;

TEST(BloomTest, BasicRoundTrip) {
    BloomBuilder builder(100, 10);

    std::vector<std::string> keys;
    for (int i = 0; i < 100; ++i) {
        keys.push_back("key_" + std::to_string(i));
    }
    for (const auto& key : keys) {
        builder.add_key(key);
    }
    EXPECT_EQ(builder.num_keys_added(), 100u);

    std::string data = builder.finish();
    ASSERT_FALSE(data.empty());

    auto reader_or = BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    auto reader = std::move(*reader_or);

    EXPECT_EQ(reader.num_keys(), 100u);

    // All inserted keys must report may_contain == true
    for (const auto& key : keys) {
        EXPECT_TRUE(reader.may_contain(key)) << "False negative for key: " << key;
    }
}

TEST(BloomTest, NoFalseNegatives) {
    BloomBuilder builder(1000, 10);

    for (int i = 0; i < 1000; ++i) {
        builder.add_key("item_" + std::to_string(i));
    }
    std::string data = builder.finish();

    auto reader = *BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));

    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(reader.may_contain("item_" + std::to_string(i)));
    }
}

TEST(BloomTest, FalsePositiveRate) {
    constexpr int num_keys = 10000;
    constexpr int bits_per_key = 10;

    BloomBuilder builder(num_keys, bits_per_key);
    for (int i = 0; i < num_keys; ++i) {
        builder.add_key("inserted_" + std::to_string(i));
    }
    std::string data = builder.finish();

    auto reader = *BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));

    // Test with keys that were NOT inserted
    int false_positives = 0;
    constexpr int test_count = 10000;
    for (int i = 0; i < test_count; ++i) {
        if (reader.may_contain("not_inserted_" + std::to_string(i))) {
            ++false_positives;
        }
    }

    double fpr = static_cast<double>(false_positives) / test_count;
    // Blocked bloom filter with 10 bits/key should have FPR < 2%
    // Be generous with the bound to avoid flakiness
    EXPECT_LT(fpr, 0.05) << "FPR too high: " << fpr;
}

TEST(BloomTest, EmptyFilter) {
    BloomBuilder builder(100, 10);
    std::string data = builder.finish();

    auto reader = *BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));

    EXPECT_EQ(reader.num_keys(), 0u);
    // Empty filter should not match anything
    EXPECT_FALSE(reader.may_contain("any_key"));
}

TEST(BloomTest, SingleKey) {
    BloomBuilder builder(1, 10);
    builder.add_key("the_one_key");
    std::string data = builder.finish();

    auto reader = *BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));

    EXPECT_TRUE(reader.may_contain("the_one_key"));
    EXPECT_EQ(reader.num_keys(), 1u);
}

TEST(BloomTest, InvalidDataTooSmall) {
    std::byte tiny[4] = {};
    auto result = BloomReader::open(std::span<const std::byte>(tiny, 4));
    EXPECT_FALSE(result.ok());
}

TEST(BloomTest, FalsePositiveRateEstimate) {
    BloomBuilder builder(1000, 10);
    for (int i = 0; i < 1000; ++i) {
        builder.add_key("k" + std::to_string(i));
    }
    std::string data = builder.finish();

    auto reader = *BloomReader::open(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));

    double fpr = reader.false_positive_rate();
    // With 10 bits/key, theoretical FPR for blocked bloom is around 1.5%
    EXPECT_GT(fpr, 0.0);
    EXPECT_LT(fpr, 0.10);
}
