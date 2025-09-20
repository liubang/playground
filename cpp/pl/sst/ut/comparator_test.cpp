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

#include "cpp/pl/sst/comparator.h"
#include <gtest/gtest.h>
#include <memory>

namespace pl {

class ComparatorTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ComparatorTest, compara) {
    auto comparator = std::make_unique<BytewiseComparator>();

    std::string_view b1 = "abc";
    std::string_view b2 = "abd";
    std::string_view b3 = "abc";
    std::string_view b4 = "aba";
    std::string_view b5;
    std::string_view b6 = "abcccc";

    EXPECT_TRUE(comparator->compare(b1, b2) < 0);
    EXPECT_TRUE(comparator->compare(b1, b3) == 0);
    EXPECT_TRUE(comparator->compare(b1, b4) > 0);
    EXPECT_TRUE(comparator->compare(b1, b5) > 0);
    EXPECT_TRUE(comparator->compare(b1, b6) < 0);
}

TEST_F(ComparatorTest, findShortestSeparator) {
    auto comparator = std::make_unique<BytewiseComparator>();

    {
        std::string start = "hello";
        std::string limit = "hellos";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("hello", start);
    }

    {
        std::string start = "hello";
        std::string limit = "hello";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("hello", start);
    }

    {
        std::string start = "hello";
        std::string limit = "hellp";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("hello", start);
    }

    {
        std::string start = "hello";
        std::string limit = "hellqr";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("hellp", start);
    }

    {
        std::string start = "hello";
        std::string limit = "i";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("hello", start);
    }

    {
        std::string start = "hello";
        std::string limit = "jqsfadq";
        comparator->findShortestSeparator(&start, limit);
        EXPECT_EQ("i", start);
    }
}

TEST_F(ComparatorTest, findShortSucessor) {
    auto comparator = std::make_unique<BytewiseComparator>();
    {
        std::string key = "hello";
        comparator->findShortSucessor(&key);
        EXPECT_EQ("i", key);
    }
    {
        std::string key;
        key.push_back(0xff);
        key.push_back(0xff);
        key.push_back(0xff);
        key.push_back('a');
        key.push_back(0xff);
        comparator->findShortSucessor(&key);
        EXPECT_EQ(4, key.size());
        // 需要注意的是，key[0]默认是char类型，带符号，这里需要转成无符号才能直接跟0xff做比较
        EXPECT_EQ(0xff, static_cast<uint8_t>(key[0]));
        EXPECT_EQ(0xff, static_cast<uint8_t>(key[1]));
        EXPECT_EQ(0xff, static_cast<uint8_t>(key[2]));
        EXPECT_EQ('b', key[3]);
    }
}

} // namespace pl
