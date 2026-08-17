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

// Hybrid 检索管道单测：BM25 路、向量路、RRF 融合、tombstone 收敛、权重开关、
// 后置 filter、cosine metric。

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/analysis/analyzer.h"
#include "cpp/pl/minisearch/core/collection.h"
#include "cpp/pl/minisearch/faiss_index.h"
#include "cpp/pl/minisearch/index/inverted_index.h"
#include "cpp/pl/minisearch/server/search_pipeline.h"

namespace pma = pl::minisearch::analysis;
namespace pmc = pl::minisearch::core;
namespace pmi = pl::minisearch::index;
namespace pmsrv = pl::minisearch::server;
using pl::minisearch::FaissIndex;
using pl::minisearch::VectorMetric;

namespace {

constexpr int kDims = 4;

pmc::Schema text_schema() {
    pmc::Schema schema;
    schema.default_analyzer = "raw";
    pmc::FieldDef title;
    title.name = "title";
    title.type = pmc::FieldType::kText;
    title.indexed = true;
    title.stored = true;
    schema.fields["title"] = title;
    pmc::FieldDef vec;
    vec.name = "v";
    vec.type = pmc::FieldType::kVector;
    vec.dims = kDims;
    vec.indexed = false;
    schema.fields["v"] = vec;
    return schema;
}

pmc::Document make_doc(const std::string& id, const std::string& title, float x) {
    pmc::Document doc;
    doc.id = id;
    doc.version = 1;
    doc.fields["title"] = title;
    doc.fields["v"] = std::vector<float>{x, 0.0f, 0.0f, 0.0f};
    return doc;
}

// 模拟 server 写入路径：core upsert + 向量 + 倒排。
struct Fixture {
    pma::RawAnalyzer analyzer;
    pmi::InvertedIndex inverted;
    FaissIndex vector_index{kDims, "Flat"};
    pmc::Collection docs{"kb", text_schema()};

    int64_t put(const pmc::Document& doc) {
        auto result = docs.Upsert(doc);
        EXPECT_TRUE(result.ok);
        const auto& title = std::get<std::string>(doc.fields.at("title"));
        inverted.Add(result.internal_docid, analyzer.Analyze(title));
        const auto& vec = std::get<std::vector<float>>(doc.fields.at("v"));
        vector_index.add(result.internal_docid, vec.data());
        return result.internal_docid;
    }

    pmsrv::HybridResult search(const std::string& text,
                               const std::vector<float>& embedding,
                               int top_k,
                               const pmsrv::HybridOptions& options,
                               const std::function<bool(const pmc::Document&)>& filter = nullptr) {
        return pmsrv::HybridSearch(
            analyzer, inverted, &vector_index, docs, text, embedding, top_k, options, filter);
    }
};

} // namespace

TEST(HybridSearchTest, Bm25Only) {
    Fixture f;
    const int64_t d1 = f.put(make_doc("doc1", "presto query tuning", 0.0f));
    f.put(make_doc("doc2", "loom architecture", 0.0f));

    pmsrv::HybridOptions options;
    options.vector_weight = 0.0; // 关闭向量路
    auto result = f.search("presto tuning", {}, 5, options);
    ASSERT_EQ(result.hits.size(), 1u);
    EXPECT_EQ(result.hits[0].docid, d1);
    EXPECT_TRUE(result.bm25_active);
    EXPECT_FALSE(result.vector_active);
}

TEST(HybridSearchTest, VectorOnly) {
    Fixture f;
    const int64_t d1 = f.put(make_doc("doc1", "presto", 1.0f));
    f.put(make_doc("doc2", "presto", 0.0f)); // 相同文本，不同向量

    pmsrv::HybridOptions options;
    options.bm25_weight = 0.0; // 关闭 BM25 路
    const std::vector<float> query = {0.9f, 0.0f, 0.0f, 0.0f};
    auto result = f.search("", query, 5, options);
    ASSERT_EQ(result.hits.size(), 2u);   // 两篇都进向量路
    EXPECT_EQ(result.hits[0].docid, d1); // 最近邻排第一
    EXPECT_FALSE(result.bm25_active);
    EXPECT_TRUE(result.vector_active);
}

TEST(HybridSearchTest, RrfPrefersBothRoadHits) {
    Fixture f;
    const int64_t d1 = f.put(make_doc("doc1", "presto tuning", 1.0f));
    f.put(make_doc("doc2", "presto tuning", 0.0f)); // BM25 同分，向量远
    f.put(make_doc("doc3", "other", 0.9f));         // 仅向量命中

    const std::vector<float> query = {1.0f, 0.0f, 0.0f, 0.0f};
    auto result = f.search("presto tuning", query, 3, {});
    ASSERT_FALSE(result.hits.empty());
    // doc1 两路都命中（rank 双第一），RRF 分最高
    EXPECT_EQ(result.hits[0].docid, d1);
}

TEST(HybridSearchTest, TombstonedExcluded) {
    Fixture f;
    f.put(make_doc("doc1", "presto tuning", 1.0f));
    const int64_t d2 = f.put(make_doc("doc2", "presto tuning", 0.9f));
    f.docs.Delete("doc2");

    const std::vector<float> query = {0.9f, 0.0f, 0.0f, 0.0f};
    auto result = f.search("presto", query, 5, {});
    EXPECT_FALSE(result.hits.empty());
    for (const auto& hit : result.hits) {
        EXPECT_NE(hit.docid, d2);
    }
}

TEST(HybridSearchTest, EmptyQueryReturnsEmpty) {
    Fixture f;
    f.put(make_doc("doc1", "presto", 1.0f));
    auto result = f.search("", {}, 5, {});
    EXPECT_TRUE(result.hits.empty());
}

TEST(HybridSearchTest, FilterAppliedAtConvergence) {
    Fixture f;
    f.put(make_doc("doc1", "presto tuning", 1.0f));
    const int64_t d2 = f.put(make_doc("doc2", "presto guide", 0.0f));

    // 只保留 title 含 "guide" 的文档
    auto filter = [](const pmc::Document& doc) {
        return std::get<std::string>(doc.fields.at("title")).find("guide") != std::string::npos;
    };
    auto result = f.search("presto", {}, 5, {}, filter);
    ASSERT_EQ(result.hits.size(), 1u);
    EXPECT_EQ(result.hits[0].docid, d2);
}

TEST(HybridSearchTest, CosineMetricRanksByDirection) {
    // cosine 下同方向不同模长的向量等距；L2 下不等距。
    pma::RawAnalyzer analyzer;
    pmi::InvertedIndex inverted;
    FaissIndex cosine_index{kDims, "Flat", VectorMetric::kCosine};
    pmc::Collection docs{"kb", text_schema()};

    auto put = [&](const std::string& id, float scale) {
        pmc::Document doc = make_doc(id, "t", 0.0f);
        doc.fields["v"] = std::vector<float>{scale, 0.0f, 0.0f, 0.0f};
        auto result = docs.Upsert(doc);
        EXPECT_TRUE(result.ok);
        const auto& vec = std::get<std::vector<float>>(doc.fields.at("v"));
        cosine_index.add(result.internal_docid, vec.data());
        return result.internal_docid;
    };
    const int64_t near_small = put("small", 1.0f);  // 与 query 同向
    const int64_t near_large = put("large", 100.f); // 同向但模长 100 倍
    put("orth", 0.0f);                              // 零向量，无关

    const std::vector<float> query = {1.0f, 0.0f, 0.0f, 0.0f};
    auto results = cosine_index.search(query.data(), 3);
    ASSERT_EQ(results.size(), 3u);
    // 前两名必须是两个同向向量（cosine 相似度都约为 1），顺序不限
    EXPECT_NE(results[0].id, results[1].id);
    EXPECT_TRUE((results[0].id == near_small && results[1].id == near_large) ||
                (results[0].id == near_large && results[1].id == near_small));
    EXPECT_NEAR(results[0].distance, 1.0f, 1e-4);
    EXPECT_NEAR(results[1].distance, 1.0f, 1e-4);
}
