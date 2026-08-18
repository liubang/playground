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

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cpp/pl/minisearch/analysis/analyzer.h"
#include "cpp/pl/minisearch/core/collection.h"
#include "cpp/pl/minisearch/faiss_index.h"
#include "cpp/pl/minisearch/index/inverted_index.h"
#include "cpp/pl/minisearch/storage/checkpoint.h"

namespace pl::minisearch::server {

// One live collection: document identity (core::Collection) plus the vector
// index keyed by internal docid and the in-memory inverted index (DESIGN.md
// §5.1 M1 form).
struct CollectionEntry {
    core::Collection docs;
    std::unique_ptr<FaissIndex> index; // null when the schema has no vector field
    std::unique_ptr<index::InvertedIndex> inverted;
    // collection 默认 analyzer + 字段级 override（DESIGN.md §4.1）。
    std::unique_ptr<analysis::Analyzer> default_analyzer;
    std::map<std::string, std::unique_ptr<analysis::Analyzer>> field_analyzers;

    CollectionEntry(std::string name, core::Schema schema);

    // 查询侧 analyzer（collection 默认）。
    const analysis::Analyzer& QueryAnalyzer() const { return *default_analyzer; }

    // 文本索引写入：每个 indexed text 字段用自己的 analyzer（字段 override
    // 或 collection 默认）分词，合并为单条 token 流后一次 Add，保证
    // BM25 的 doc length 为全文字符总 token 数。
    void IndexText(int64_t docid, const core::Document& doc);

    // 向量索引写入：失败打 WARNING——文档仍可按 id/倒排检索，向量缺失
    // 到下次 upsert 或重建（崩溃一致性窗口，DESIGN.md §5.1）。
    void IndexVector(int64_t docid, const core::Document& doc);

private:
    const analysis::Analyzer& AnalyzerFor(const core::FieldDef& def) const;
};

// Registry of live collections with optional persistence (DESIGN.md §5.1):
//   - store != nullptr: Create/Drop write through; a background thread
//     checkpoints dirty collections (pending >= 1000 writes or >= 300 s);
//     LoadFromDisk() rebuilds all collections at startup — documents and the
//     FAISS index come from the checkpoint files, the inverted index is
//     rebuilt by re-analyzing restored documents (which also compacts away
//     tombstoned postings).
//   - store == nullptr: pure in-memory mode (tests, ephemeral runs).
// The registry owns its CheckpointStore so destruction order is always
// safe: worker thread joined first, store destroyed afterwards.
class CollectionRegistry {
public:
    struct CreateResult {
        bool ok = false;
        std::string error;
    };

    explicit CollectionRegistry(std::unique_ptr<storage::CheckpointStore> store = nullptr,
                                std::string index_type = "Flat");

    CreateResult Create(const proto::CollectionSpec& spec);

    // 将 collection 迁移到另一个租户的 registry：按相同 schema 在目标侧重建，
    // 逐文档复制并重建向量/倒排索引（向量随文档字段拷贝，无需重新 embedding），
    // 全部成功后从本 registry 删除（含持久化数据）。目标侧已有同名 collection
    // 或复制中途失败时回滚目标侧，源侧保持不变。
    struct MoveResult {
        bool ok = false;
        std::string error;
        size_t documents = 0;
    };
    MoveResult MoveTo(CollectionRegistry& dst, const std::string& name);

    std::shared_ptr<CollectionEntry> Find(const std::string& name) const;

    bool Drop(const std::string& name);

    std::vector<std::pair<std::string, size_t>> ListWithCounts() const;

    struct Stats {
        size_t collections = 0;
        size_t active_documents = 0;
        // 顶层文档数：markdown 导入的文档按 "<name>#chunk_" 前缀去重后的数量
        size_t top_level_documents = 0;
    };
    Stats GetStats() const;

    // Restores every persisted collection. Returns the number restored.
    size_t LoadFromDisk();

    // 立即对单个 collection 做 checkpoint（测试与管理面使用）。
    // collection 不存在或持久化未开启/写盘失败时返回 false。
    bool Checkpoint(const std::string& name);

    // Background checkpoint loop; stops when stop becomes true or the
    // registry is destroyed.
    void StartCheckpointLoop(std::shared_ptr<std::atomic<bool>> stop);

    ~CollectionRegistry();

    // First vector field of the schema, or nullptr. A collection has at most
    // one vector index in M0.
    static const core::FieldDef* VectorField(const core::Schema& schema);

private:
    CreateResult CreateLocked(const proto::CollectionSpec& spec, bool persist);
    // force=true 忽略阈值，落盘所有 dirty collection（进程退出前的 final
    // flush 必须无条件写盘，否则未达阈值的写入会丢）。
    void CheckpointAll(bool force);

    std::unique_ptr<storage::CheckpointStore> store_;
    std::string index_type_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<CollectionEntry>> entries_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_checkpoint_;
    std::shared_ptr<std::atomic<bool>> stop_;
    std::thread worker_;
};

} // namespace pl::minisearch::server
