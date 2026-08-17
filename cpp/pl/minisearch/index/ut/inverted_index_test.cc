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

// 倒排索引单测：term 频率、文档统计、持久化 round-trip。

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/analysis/analyzer.h"
#include "cpp/pl/minisearch/index/inverted_index.h"

namespace pma = pl::minisearch::analysis;
namespace pmi = pl::minisearch::index;

namespace {

std::vector<pma::Token> raw_tokens(const std::string& text) {
    pma::RawAnalyzer analyzer;
    return analyzer.Analyze(text);
}

} // namespace

TEST(InvertedIndexTest, AddAndFind) {
    pmi::InvertedIndex index;
    index.Add(0, raw_tokens("hello world hello"));
    index.Add(1, raw_tokens("world peace"));

    auto hello = index.Find("hello");
    ASSERT_EQ(hello.size(), 1u);
    EXPECT_EQ(hello[0].docid, 0);
    EXPECT_EQ(hello[0].tf, 2u);

    auto world = index.Find("world");
    ASSERT_EQ(world.size(), 2u);
    EXPECT_TRUE(index.Find("missing").empty());
}

TEST(InvertedIndexTest, DocStatistics) {
    pmi::InvertedIndex index;
    index.Add(0, raw_tokens("a b c"));
    index.Add(1, raw_tokens("a b"));
    EXPECT_EQ(index.DocCount(), 2);
    EXPECT_EQ(index.DocLength(0), 3u);
    EXPECT_EQ(index.DocLength(1), 2u);
    EXPECT_DOUBLE_EQ(index.AvgDocLength(), 2.5);
}

TEST(InvertedIndexTest, PersistenceRoundTrip) {
    pmi::InvertedIndex index;
    index.Add(0, raw_tokens("查询 性能 调优 查询"));
    index.Add(1, raw_tokens("查询 优化"));

    const std::string path = "/tmp/minisearch_inv_test.bin";
    ASSERT_TRUE(index.Save(path));

    pmi::InvertedIndex restored;
    ASSERT_TRUE(restored.Load(path));
    EXPECT_EQ(restored.DocCount(), 2);
    EXPECT_EQ(restored.DocLength(0), 4u);
    auto hits = restored.Find("查询");
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].tf, 2u);
    EXPECT_EQ(hits[1].docid, 1);
    ::unlink(path.c_str());
}
