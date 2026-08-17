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
// Created: 2026/05/14 10:44

#include "cpp/pl/minisearch/faiss_index.h"

#include <cmath>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#include <fstream>

namespace pl::minisearch {

FaissIndex::FaissIndex(int dimension, const std::string& index_type, VectorMetric metric)
    : dimension_(dimension), index_type_(index_type), metric_(metric) {
    // cosine 与 dot_product 都用内积索引（cosine 额外做归一化，见 prepare）
    const faiss::MetricType faiss_metric =
        metric_ == VectorMetric::kL2 ? faiss::METRIC_L2 : faiss::METRIC_INNER_PRODUCT;
    // index_factory 根据描述字符串创建对应的索引结构
    std::unique_ptr<faiss::Index> base(
        faiss::index_factory(dimension, index_type.c_str(), faiss_metric));
    // 用 IndexIDMap 包装，支持自定义 id
    index_ = std::make_unique<faiss::IndexIDMap>(base.release());
    // IndexIDMap 接管了 base 的所有权，own_fields 默认为 false，需要手动设置
    index_->own_fields = true;
}

std::vector<float> FaissIndex::prepare(const float* vec, int count) const {
    std::vector<float> out(vec, vec + static_cast<size_t>(count) * dimension_);
    if (metric_ != VectorMetric::kCosine) {
        return out;
    }
    for (int i = 0; i < count; ++i) {
        float* row = out.data() + static_cast<size_t>(i) * dimension_;
        double norm = 0.0;
        for (int j = 0; j < dimension_; ++j) {
            norm += static_cast<double>(row[j]) * row[j];
        }
        norm = std::sqrt(norm);
        if (norm <= 0.0) {
            continue; // 零向量保持原样（内积恒 0）
        }
        for (int j = 0; j < dimension_; ++j) {
            row[j] = static_cast<float>(row[j] / norm);
        }
    }
    return out;
}

FaissIndex::~FaissIndex() = default;

bool FaissIndex::add(int64_t id, const float* embedding) {
    const std::vector<float> prepared = prepare(embedding, 1);
    std::lock_guard<std::mutex> lock(mu_);
    try {
        index_->add_with_ids(1, prepared.data(), &id);
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

int FaissIndex::add_batch(const std::vector<int64_t>& ids, const float* embeddings, int count) {
    if (static_cast<int>(ids.size()) != count) {
        return 0;
    }
    const std::vector<float> prepared = prepare(embeddings, count);
    std::lock_guard<std::mutex> lock(mu_);
    try {
        index_->add_with_ids(count, prepared.data(), ids.data());
        return count;
    } catch (const std::exception& e) {
        return 0;
    }
}

std::vector<SearchResult> FaissIndex::search(const float* query, int top_k) const {
    std::vector<float> distances(top_k);
    std::vector<int64_t> labels(top_k);

    const std::vector<float> prepared = prepare(query, 1);
    {
        std::lock_guard<std::mutex> lock(mu_);
        index_->search(1, prepared.data(), top_k, distances.data(), labels.data());
    }

    std::vector<SearchResult> results;
    results.reserve(top_k);
    for (size_t i = 0; i < static_cast<size_t>(top_k); ++i) {
        // faiss 用 -1 表示无效结果（向量数不足 top_k 时）
        if (labels[static_cast<int>(i)] < 0) {
            break;
        }
        results.push_back(
            {.id = labels[static_cast<int>(i)], .distance = distances[static_cast<int>(i)]});
    }
    return results;
}

bool FaissIndex::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        faiss::write_index(index_.get(), path.c_str());
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

bool FaissIndex::load(const std::string& path) {
    try {
        std::unique_ptr<faiss::Index> loaded(faiss::read_index(path.c_str()));
        auto* id_map = dynamic_cast<faiss::IndexIDMap*>(loaded.get());
        if (id_map == nullptr) {
            return false;
        }
        // 维度必须与 schema 锁定值一致，拒绝错配的 checkpoint 文件
        if (id_map->d != dimension_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        loaded.release();
        index_.reset(id_map);
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

int64_t FaissIndex::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return index_->ntotal;
}

bool FaissIndex::is_trained() const {
    std::lock_guard<std::mutex> lock(mu_);
    return index_->is_trained;
}

} // namespace pl::minisearch
