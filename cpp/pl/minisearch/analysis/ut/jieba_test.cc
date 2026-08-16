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

// Smoke tests for the @jieba_cpp integration (DESIGN.md §15-4): dictionary
// loading, CJK segmentation and mixed ASCII/CJK handling.

#include <algorithm>
#include <gtest/gtest.h>
#include <jieba.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> toVec(const std::vector<std::string_view>& tokens) {
    return {tokens.begin(), tokens.end()};
}

bool contains(const std::vector<std::string>& tokens, const std::string& word) {
    return std::find(tokens.begin(), tokens.end(), word) != tokens.end();
}

} // namespace

TEST(JiebaTest, CutCjk) {
    Jieba::Jieba jieba;
    EXPECT_EQ(toVec(jieba.cut("南京市长江大桥")), (std::vector<std::string>{"南京市", "长江大桥"}));
    EXPECT_EQ(toVec(jieba.cut("他来到了网易杭研大厦")),
              (std::vector<std::string>{"他", "来到", "了", "网易", "杭研", "大厦"}));
}

TEST(JiebaTest, CutAsciiAndMixed) {
    Jieba::Jieba jieba;
    EXPECT_EQ(toVec(jieba.cut("5G网络")), (std::vector<std::string>{"5G", "网络"}));
    EXPECT_EQ(toVec(jieba.cut("你好，世界")), (std::vector<std::string>{"你好", "世界"}));
    EXPECT_EQ(toVec(jieba.cut("hello,world")), (std::vector<std::string>{"hello", "world"}));
}

TEST(JiebaTest, CutTechnicalText) {
    Jieba::Jieba jieba;
    auto tokens = toVec(jieba.cut("presto 查询性能调优：join 阶段 CPU 热点排查"));
    EXPECT_TRUE(contains(tokens, "presto"));
    EXPECT_TRUE(contains(tokens, "join"));
    EXPECT_TRUE(contains(tokens, "CPU"));
    EXPECT_TRUE(contains(tokens, "查询"));
    EXPECT_TRUE(contains(tokens, "热点"));
}

TEST(JiebaTest, CutForSearchEmitsSubwords) {
    Jieba::Jieba jieba;
    auto coarse = toVec(jieba.cut("北京邮电大学"));
    auto search = toVec(jieba.cutForSearch("北京邮电大学"));
    EXPECT_LT(coarse.size(), search.size());
}
