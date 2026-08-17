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

#pragma once

#include <cstdint>
#include <faiss/IndexIDMap.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pl::minisearch {

// 单条检索结果
struct SearchResult {
    int64_t id;
    float distance;
};

// 向量距离度量（schema 中的 metric 字段，DESIGN.md §4.1）。
// cosine 以 L2 归一化 + 内积实现（写入与查询两侧都归一化）。
enum class VectorMetric {
    kL2,
    kInnerProduct,
    kCosine,
};

// 线程安全的 faiss 索引封装，支持增量写入和并发读取
class FaissIndex {
public:
    // dimension: 向量维度
    // index_type: faiss 索引描述字符串，如 "Flat", "IVF256,Flat", "HNSW32"
    // metric: 距离度量；cosine 在 add/search 时归一化并用内积索引
    FaissIndex(int dimension,
               const std::string& index_type,
               VectorMetric metric = VectorMetric::kL2);
    ~FaissIndex();

    FaissIndex(const FaissIndex&) = delete;
    FaissIndex& operator=(const FaissIndex&) = delete;

    // 添加单条向量，id 由调用方指定
    bool add(int64_t id, const float* embedding);

    // 批量添加
    int add_batch(const std::vector<int64_t>& ids, const float* embeddings, int count);

    // 检索 top-k 最近邻
    std::vector<SearchResult> search(const float* query, int top_k) const;

    // 持久化到磁盘
    bool save(const std::string& path) const;

    // 从磁盘加载（替换当前索引）
    bool load(const std::string& path);

    // 索引中的向量总数
    int64_t size() const;

    int dimension() const { return dimension_; }
    const std::string& index_type() const { return index_type_; }
    VectorMetric metric() const { return metric_; }
    bool is_trained() const;

private:
    // 按 metric 预处理（cosine: L2 归一化拷贝）；其他 metric 原样返回别名。
    std::vector<float> prepare(const float* vec, int count) const;

    int dimension_;
    std::string index_type_;
    VectorMetric metric_;
    std::unique_ptr<faiss::IndexIDMap> index_;
    mutable std::mutex mu_;
};

} // namespace pl::minisearch
