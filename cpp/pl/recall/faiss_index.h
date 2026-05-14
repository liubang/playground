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
#include <unordered_map>
#include <vector>

namespace pl::recall {

// 单条召回结果
struct RecallResult {
    int64_t id;
    float distance;
};

// 线程安全的 faiss 索引封装，支持增量写入和并发读取
class FaissIndex {
public:
    // dimension: 向量维度
    // index_type: faiss 索引描述字符串，如 "Flat", "IVF256,Flat", "HNSW32"
    FaissIndex(int dimension, const std::string& index_type);
    ~FaissIndex();

    FaissIndex(const FaissIndex&) = delete;
    FaissIndex& operator=(const FaissIndex&) = delete;

    // 添加单条向量，id 由调用方指定
    bool add(int64_t id, const float* embedding);

    // 批量添加
    int add_batch(const std::vector<int64_t>& ids, const float* embeddings, int count);

    // 检索 top-k 最近邻
    std::vector<RecallResult> search(const float* query, int top_k) const;

    // 持久化到磁盘
    bool save(const std::string& path) const;

    // 从磁盘加载（替换当前索引）
    bool load(const std::string& path);

    // 索引中的向量总数
    int64_t size() const;

    int dimension() const { return dimension_; }
    const std::string& index_type() const { return index_type_; }
    bool is_trained() const;

private:
    int dimension_;
    std::string index_type_;
    std::unique_ptr<faiss::IndexIDMap> index_;
    mutable std::mutex mu_;
};

// id <-> table_id 的双向映射
class IdMapper {
public:
    // 分配或获取已有的数值 id
    int64_t get_or_assign(const std::string& table_id);

    // 数值 id -> table_id
    std::string get_table_id(int64_t id) const;

    // 持久化
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    int64_t size() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, int64_t> table_to_id_;
    std::unordered_map<int64_t, std::string> id_to_table_;
    int64_t next_id_ = 0;
};

} // namespace pl::recall
