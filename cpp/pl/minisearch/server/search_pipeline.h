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

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/analysis/analyzer.h"
#include "cpp/pl/minisearch/core/collection.h"
#include "cpp/pl/minisearch/faiss_index.h"
#include "cpp/pl/minisearch/index/inverted_index.h"

namespace pl::minisearch::server {

// Hybrid 检索管道（DESIGN.md §7）：BM25 倒排路 + ANN 向量路 → RRF 融合 →
// tombstone 收敛 + 后置 filter。返回按融合分排序的 (docid, score)。
struct HybridHit {
    int64_t docid = -1;
    double score = 0.0;
};

struct HybridOptions {
    double bm25_weight = 1.0;
    double vector_weight = 1.0;
    double rrf_k = 60.0;
    double bm25_k1 = 1.2;
    double bm25_b = 0.75;
};

struct HybridResult {
    std::vector<HybridHit> hits;
    // 各路是否实际参与了融合（handler 据此计算 degraded 标记）。
    bool bm25_active = false;
    bool vector_active = false;
};

// filter: 后置过滤谓词（DESIGN.md §7.3 M1）；nullptr 表示不过滤。
// 在收敛阶段与 tombstone 检查一起应用。
HybridResult HybridSearch(const analysis::Analyzer& analyzer,
                          const index::InvertedIndex& inverted,
                          const FaissIndex* vector_index,
                          const core::Collection& docs,
                          const std::string& query_text,
                          const std::vector<float>& query_embedding,
                          int top_k,
                          const HybridOptions& options,
                          const std::function<bool(const core::Document&)>& filter);

} // namespace pl::minisearch::server
