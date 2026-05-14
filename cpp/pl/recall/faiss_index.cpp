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

#include "cpp/pl/recall/faiss_index.h"

#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#include <fstream>

namespace pl::recall {

FaissIndex::FaissIndex(int dimension, const std::string& index_type)
    : dimension_(dimension), index_type_(index_type) {
    // index_factory 根据描述字符串创建对应的索引结构
    std::unique_ptr<faiss::Index> base(faiss::index_factory(dimension, index_type.c_str()));
    // 用 IndexIDMap 包装，支持自定义 id
    index_ = std::make_unique<faiss::IndexIDMap>(base.release());
    // IndexIDMap 接管了 base 的所有权，own_fields 默认为 false，需要手动设置
    index_->own_fields = true;
}

FaissIndex::~FaissIndex() = default;

bool FaissIndex::add(int64_t id, const float* embedding) {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        index_->add_with_ids(1, embedding, &id);
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

int FaissIndex::add_batch(const std::vector<int64_t>& ids, const float* embeddings, int count) {
    if (static_cast<int>(ids.size()) != count) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mu_);
    try {
        index_->add_with_ids(count, embeddings, ids.data());
        return count;
    } catch (const std::exception& e) {
        return 0;
    }
}

std::vector<RecallResult> FaissIndex::search(const float* query, int top_k) const {
    std::vector<float> distances(top_k);
    std::vector<int64_t> labels(top_k);

    {
        std::lock_guard<std::mutex> lock(mu_);
        index_->search(1, query, top_k, distances.data(), labels.data());
    }

    std::vector<RecallResult> results;
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
        std::lock_guard<std::mutex> lock(mu_);
        loaded.release();
        index_.reset(id_map);
        dimension_ = index_->d;
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

// =========================================================================
// IdMapper
// =========================================================================

int64_t IdMapper::get_or_assign(const std::string& table_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = table_to_id_.find(table_id);
    if (it != table_to_id_.end()) {
        return it->second;
    }
    int64_t id = next_id_++;
    table_to_id_[table_id] = id;
    id_to_table_[id] = table_id;
    return id;
}

int64_t IdMapper::assign_new(const std::string& table_id) {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t id = next_id_++;
    // 不更新 table_to_id_（保留首次分配的映射），只建立 id -> table_id 反查
    id_to_table_[id] = table_id;
    // 如果是首次出现，也写入正向映射
    if (table_to_id_.find(table_id) == table_to_id_.end()) {
        table_to_id_[table_id] = id;
    }
    return id;
}

std::string IdMapper::get_table_id(int64_t id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = id_to_table_.find(id);
    if (it != id_to_table_.end()) {
        return it->second;
    }
    return {};
}

bool IdMapper::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return false;
    }
    // 格式: [next_id][count][id, len, table_id_bytes]...
    ofs.write(reinterpret_cast<const char*>(&next_id_), sizeof(next_id_));
    auto count = static_cast<int64_t>(id_to_table_.size());
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [id, table_id] : id_to_table_) {
        ofs.write(reinterpret_cast<const char*>(&id), sizeof(id));
        auto len = static_cast<int32_t>(table_id.size());
        ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        ofs.write(table_id.data(), len);
    }
    return ofs.good();
}

bool IdMapper::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    table_to_id_.clear();
    id_to_table_.clear();

    ifs.read(reinterpret_cast<char*>(&next_id_), sizeof(next_id_));
    int64_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
    for (int64_t i = 0; i < count; ++i) {
        int64_t id = 0;
        ifs.read(reinterpret_cast<char*>(&id), sizeof(id));
        int32_t len = 0;
        ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
        std::string table_id(static_cast<size_t>(len), '\0');
        ifs.read(table_id.data(), len);
        table_to_id_[table_id] = id;
        id_to_table_[id] = std::move(table_id);
    }
    return ifs.good();
}

int64_t IdMapper::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int64_t>(id_to_table_.size());
}

} // namespace pl::recall
