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

// analyzer 适配层单测：raw 空白切分、jieba 中文/混合分词、偏移正确性。

#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/minisearch/analysis/analyzer.h"

namespace pma = pl::minisearch::analysis;

namespace {

std::string join_terms(const std::vector<pma::Token>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            out += "|";
        }
        out += tokens[i].term;
    }
    return out;
}

} // namespace

TEST(AnalyzerTest, RawSplitsOnWhitespace) {
    pma::RawAnalyzer analyzer;
    auto tokens = analyzer.Analyze("hello   world");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].term, "hello");
    EXPECT_EQ(tokens[0].begin, 0u);
    EXPECT_EQ(tokens[0].end, 5u);
    EXPECT_EQ(tokens[1].term, "world");
    EXPECT_EQ(tokens[1].begin, 8u);
    EXPECT_EQ(tokens[1].pos, 1u);
    EXPECT_TRUE(analyzer.Analyze("   ").empty());
}

TEST(AnalyzerTest, JiebaCutCjkAndAscii) {
    pma::JiebaAnalyzer analyzer;
    auto tokens = analyzer.Analyze("南京市长江大桥");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(join_terms(tokens), "南京市|长江大桥");

    tokens = analyzer.Analyze("5G网络");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(join_terms(tokens), "5G|网络");
}

TEST(AnalyzerTest, JiebaOffsetsReconstructOriginal) {
    pma::JiebaAnalyzer analyzer;
    const std::string text = "presto 查询性能调优";
    auto tokens = analyzer.Analyze(text);
    ASSERT_FALSE(tokens.empty());
    // 非重叠 token 的切片拼接还原原文中的连续片段
    for (const auto& token : tokens) {
        ASSERT_LT(token.begin, token.end);
        ASSERT_LE(token.end, text.size());
        EXPECT_EQ(text.substr(token.begin, token.end - token.begin), token.term);
    }
    EXPECT_EQ(tokens.front().begin, 0u);
    EXPECT_EQ(tokens.back().end, text.size());
}

TEST(AnalyzerTest, CreateAnalyzerFactory) {
    EXPECT_NE(pma::CreateAnalyzer("raw"), nullptr);
    EXPECT_NE(pma::CreateAnalyzer("cjk_jieba"), nullptr);
    EXPECT_NE(pma::CreateAnalyzer(""), nullptr);
}
