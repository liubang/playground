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

#include "cpp/pl/minisearch/server/search_pipeline.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_set>

namespace pl::minisearch::server {

namespace {

// BM25 打分（DESIGN.md §7.2）。返回 docid -> score 的 top 排序结果。
std::vector<std::pair<int64_t, double>> Bm25Search(const index::InvertedIndex& inverted,
                                                   const std::vector<analysis::Token>& tokens,
                                                   int top_k,
                                                   double k1,
                                                   double b) {
    const int64_t n = inverted.DocCount();
    const double avgdl = inverted.AvgDocLength();
    if (n == 0 || avgdl <= 0.0 || tokens.empty()) {
        return {};
    }
    // 同义词/重复 term 折叠为 term 集合
    std::map<std::string, int> term_counts;
    for (const auto& token : tokens) {
        ++term_counts[token.term];
    }

    std::map<int64_t, double> scores;
    for (const auto& [term, query_tf] : term_counts) {
        const auto posting = inverted.Find(term);
        const int64_t df = static_cast<int64_t>(posting.size());
        if (df == 0) {
            continue;
        }
        const double idf = std::log(1.0 + (static_cast<double>(n) - df + 0.5) / (df + 0.5));
        for (const auto& entry : posting) {
            const uint32_t dl = inverted.DocLength(entry.docid);
            if (dl == 0) {
                continue;
            }
            const double norm = 1.0 - b + b * (static_cast<double>(dl) / avgdl);
            const double denom = entry.tf + k1 * norm;
            if (denom <= 0.0) {
                continue;
            }
            scores[entry.docid] += idf * (entry.tf * (k1 + 1.0)) / denom * query_tf;
        }
    }

    std::vector<std::pair<int64_t, double>> ranked(scores.begin(), scores.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    if (ranked.size() > static_cast<size_t>(top_k)) {
        ranked.resize(static_cast<size_t>(top_k));
    }
    return ranked;
}

// 向量路检索。取数时跳过 tombstoned 文档（它们的向量要等 checkpoint
// 重建才物理清除），并多取余量补偿后置 filter 的损耗。
std::vector<std::pair<int64_t, double>> VectorSearch(const FaissIndex* index,
                                                     const std::unordered_set<int64_t>& tombstones,
                                                     const std::vector<float>& embedding,
                                                     int top_k) {
    if (index == nullptr || embedding.empty()) {
        return {};
    }
    const int fetch = top_k + static_cast<int>(tombstones.size()) + 10;
    auto results = index->search(embedding.data(), fetch);
    std::vector<std::pair<int64_t, double>> ranked;
    ranked.reserve(results.size());
    for (const auto& hit : results) {
        if (tombstones.count(hit.id) > 0) {
            continue;
        }
        ranked.emplace_back(hit.id, hit.distance);
        if (ranked.size() >= static_cast<size_t>(top_k)) {
            break;
        }
    }
    return ranked;
}

} // namespace

HybridResult HybridSearch(const analysis::Analyzer& analyzer,
                          const index::InvertedIndex& inverted,
                          const FaissIndex* vector_index,
                          const core::Collection& docs,
                          const std::string& query_text,
                          const std::vector<float>& query_embedding,
                          int top_k,
                          const HybridOptions& options,
                          const std::function<bool(const core::Document&)>& filter) {
    const std::unordered_set<int64_t> tombstones = docs.TombstoneSnapshot();

    auto bm25 = Bm25Search(inverted,
                           analyzer.Analyze(query_text),
                           std::max(top_k * 5, 50),
                           options.bm25_k1,
                           options.bm25_b);
    auto vector = VectorSearch(vector_index, tombstones, query_embedding, std::max(top_k * 5, 50));

    // RRF 融合（DESIGN.md §7.1 第 3 步）：score = Σ w / (k + rank)，rank 从 1 起。
    std::map<int64_t, double> fused;
    auto merge = [&](const std::vector<std::pair<int64_t, double>>& ranked, double weight) {
        for (size_t rank = 0; rank < ranked.size(); ++rank) {
            fused[ranked[rank].first] += weight / (options.rrf_k + static_cast<double>(rank + 1));
        }
    };
    HybridResult result;
    result.bm25_active = options.bm25_weight > 0.0 && !bm25.empty();
    result.vector_active = options.vector_weight > 0.0 && !vector.empty();
    if (result.bm25_active) {
        merge(bm25, options.bm25_weight);
    }
    if (result.vector_active) {
        merge(vector, options.vector_weight);
    }

    // 收敛（第 5 步）：非 active 文档丢弃，随后应用后置 filter；按融合分
    // 从高到低取满 top_k 为止（filter 命中文档不足时返回更少，M1 接受）。
    std::vector<HybridHit> candidates;
    candidates.reserve(fused.size());
    for (const auto& [docid, score] : fused) {
        candidates.push_back({docid, score});
    }
    std::sort(candidates.begin(), candidates.end(), [](const HybridHit& a, const HybridHit& b) {
        return a.score > b.score;
    });
    for (const auto& cand : candidates) {
        core::Document doc;
        if (!docs.GetByInternal(cand.docid, &doc)) {
            continue; // tombstoned
        }
        if (filter && !filter(doc)) {
            continue;
        }
        result.hits.push_back(cand);
        if (result.hits.size() >= static_cast<size_t>(top_k)) {
            break;
        }
    }
    return result;
}

} // namespace pl::minisearch::server
