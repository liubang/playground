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

#include "cpp/pl/minisearch/server/collection_registry.h"

#include <butil/logging.h>
#include <chrono>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "cpp/pl/minisearch/server/codec.h"

namespace pl::minisearch::server {

namespace {

constexpr size_t kCheckpointWriteThreshold = 1000;
constexpr auto kCheckpointMaxAge = std::chrono::seconds(300);
constexpr auto kCheckpointInterval = std::chrono::seconds(60);

// 顶层文档名：markdown 导入的 chunk id 形如 "<name>#chunk_<i>"，
// 非分块文档 id 不含该后缀，整体即文档名。
std::string_view top_level_doc_name(const std::string& id) {
    const auto pos = id.rfind("#chunk_");
    return pos == std::string::npos ? std::string_view(id) : std::string_view(id).substr(0, pos);
}

VectorMetric to_vector_metric(const std::string& metric) {
    if (metric == "cosine") {
        return VectorMetric::kCosine;
    }
    if (metric == "dot_product") {
        return VectorMetric::kInnerProduct;
    }
    return VectorMetric::kL2;
}

} // namespace

CollectionEntry::CollectionEntry(std::string name, core::Schema schema)
    : docs(std::move(name), std::move(schema)),
      inverted(std::make_unique<index::InvertedIndex>()),
      default_analyzer(analysis::CreateAnalyzer(docs.schema().default_analyzer)) {
    for (const auto& [field, def] : docs.schema().fields) {
        if (def.type == core::FieldType::kText && !def.analyzer.empty() &&
            def.analyzer != docs.schema().default_analyzer) {
            field_analyzers[field] = analysis::CreateAnalyzer(def.analyzer);
        }
    }
}

const analysis::Analyzer& CollectionEntry::AnalyzerFor(const core::FieldDef& def) const {
    const auto it = field_analyzers.find(def.name);
    return it != field_analyzers.end() ? *it->second : *default_analyzer;
}

void CollectionEntry::IndexText(int64_t docid, const core::Document& doc) {
    // 合并所有 indexed text 字段的 token 流（pos 连续递增），单次 Add
    // 保证 doc_lengths_ 记录的是全文总长度。
    std::vector<analysis::Token> merged;
    uint32_t pos = 0;
    for (const auto& [name, def] : docs.schema().fields) {
        if (def.type != core::FieldType::kText || !def.indexed) {
            continue;
        }
        const auto it = doc.fields.find(name);
        if (it == doc.fields.end() || !std::holds_alternative<std::string>(it->second)) {
            continue;
        }
        for (auto token : AnalyzerFor(def).Analyze(std::get<std::string>(it->second))) {
            token.pos = pos++;
            merged.push_back(std::move(token));
        }
    }
    if (!merged.empty()) {
        inverted->Add(docid, merged);
    }
}

void CollectionEntry::IndexVector(int64_t docid, const core::Document& doc) {
    if (index == nullptr) {
        return;
    }
    const core::FieldDef* vec = CollectionRegistry::VectorField(docs.schema());
    const auto it = doc.fields.find(vec->name);
    if (it == doc.fields.end() || !std::holds_alternative<std::vector<float>>(it->second)) {
        return;
    }
    const auto& data = std::get<std::vector<float>>(it->second);
    if (!index->add(docid, data.data())) {
        LOG(WARNING) << "vector index add failed: collection=" << docs.name() << " docid=" << docid
                     << " (document stays retrievable via id/inverted index)";
    }
}

const core::FieldDef* CollectionRegistry::VectorField(const core::Schema& schema) {
    for (const auto& [name, def] : schema.fields) {
        if (def.type == core::FieldType::kVector) {
            return &def;
        }
    }
    return nullptr;
}

CollectionRegistry::CollectionRegistry(std::unique_ptr<storage::CheckpointStore> store,
                                       std::string index_type)
    : store_(std::move(store)), index_type_(std::move(index_type)) {}

CollectionRegistry::CreateResult CollectionRegistry::Create(const proto::CollectionSpec& spec) {
    std::lock_guard<std::mutex> lock(mu_);
    return CreateLocked(spec, /*persist=*/true);
}

CollectionRegistry::CreateResult CollectionRegistry::CreateLocked(const proto::CollectionSpec& spec,
                                                                  bool persist) {
    CreateResult result;
    if (!core::IsValidResourceName(spec.name())) {
        result.error = "collection name must match [A-Za-z0-9_-]{1,64}: '" + spec.name() + "'";
        return result;
    }
    core::Schema schema;
    std::string error;
    if (!ToCoreSchema(spec, &schema, &error)) {
        result.error = error;
        return result;
    }
    if (entries_.count(spec.name()) > 0) {
        result.error = "collection already exists: " + spec.name();
        return result;
    }
    auto entry = std::make_shared<CollectionEntry>(spec.name(), std::move(schema));
    if (entry->default_analyzer == nullptr) {
        result.error = "unknown default analyzer: " + spec.default_analyzer();
        return result;
    }
    if (const core::FieldDef* vec = VectorField(entry->docs.schema()); vec != nullptr) {
        entry->index =
            std::make_unique<FaissIndex>(vec->dims, index_type_, to_vector_metric(vec->metric));
    }
    if (persist && store_ != nullptr &&
        !store_->Save(spec.name(), spec, index_type_, entry->docs, entry->index.get())) {
        result.error = "failed to persist collection: " + spec.name();
        return result;
    }
    entries_.emplace(spec.name(), std::move(entry));
    last_checkpoint_[spec.name()] = std::chrono::steady_clock::now();
    result.ok = true;
    return result;
}

std::shared_ptr<CollectionEntry> CollectionRegistry::Find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second;
}

CollectionRegistry::MoveResult CollectionRegistry::MoveTo(CollectionRegistry& dst,
                                                          const std::string& name) {
    MoveResult result;
    if (&dst == this) {
        result.error = "source and target registry are the same";
        return result;
    }
    // 1. 取源 entry（shared_ptr 保活，迁移期间并发 Drop 不影响复制过程）
    std::shared_ptr<CollectionEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(name);
        if (it == entries_.end()) {
            result.error = "unknown collection: " + name;
            return result;
        }
        entry = it->second;
    }
    // 2. 目标侧按相同 schema 创建（已存在则失败，不做覆盖）
    auto created = dst.Create(ToProtoSpec(entry->docs.schema(), name));
    if (!created.ok) {
        result.error = "target: " + created.error;
        return result;
    }
    auto dstEntry = dst.Find(name);
    if (dstEntry == nullptr) {
        result.error = "target: collection vanished after create";
        return result;
    }
    // 3. 逐文档复制并重建索引；文档版本在目标侧重新分配
    std::string copyError;
    entry->docs.ForEachActive([&](const core::Document& doc) {
        if (!copyError.empty()) {
            return;
        }
        core::Document copy = doc;
        copy.version = 0;
        auto upserted = dstEntry->docs.Upsert(std::move(copy));
        if (!upserted.ok) {
            copyError = upserted.error;
            return;
        }
        core::Document stored;
        if (dstEntry->docs.GetByInternal(upserted.internal_docid, &stored)) {
            dstEntry->IndexVector(upserted.internal_docid, stored);
            dstEntry->IndexText(upserted.internal_docid, stored);
        }
        ++result.documents;
    });
    if (!copyError.empty()) {
        dst.Drop(name); // 回滚目标侧半成品，源侧不动
        result.error = "copy failed: " + copyError;
        result.documents = 0;
        return result;
    }
    // 4. 全部复制完成后删除源（含磁盘 checkpoint 数据）
    Drop(name);
    result.ok = true;
    return result;
}

bool CollectionRegistry::Drop(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    if (entries_.erase(name) == 0) {
        return false;
    }
    last_checkpoint_.erase(name);
    return store_ == nullptr || store_->Drop(name);
}

std::vector<std::pair<std::string, size_t>> CollectionRegistry::ListWithCounts() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::string, size_t>> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        out.emplace_back(name, entry->docs.ActiveCount());
    }
    return out;
}

CollectionRegistry::Stats CollectionRegistry::GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stats stats;
    stats.collections = entries_.size();
    for (const auto& [name, entry] : entries_) {
        stats.active_documents += entry->docs.ActiveCount();
        // 顶层文档数：按 "<name>#chunk_" 前缀去重
        std::unordered_set<std::string_view> names;
        entry->docs.ForEachActive(
            [&names](const core::Document& doc) { names.insert(top_level_doc_name(doc.id)); });
        stats.top_level_documents += names.size();
    }
    return stats;
}

size_t CollectionRegistry::LoadFromDisk() {
    if (store_ == nullptr) {
        return 0;
    }
    size_t restored = 0;
    for (const auto& name : store_->ListCollections()) {
        auto loaded = store_->Load(name);
        if (!loaded.has_value()) {
            continue;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (entries_.count(name) > 0) {
            continue;
        }
        auto result = CreateLocked(loaded->spec, /*persist=*/false);
        if (!result.ok) {
            LOG(WARNING) << "skipping collection " << name << ": " << result.error;
            continue;
        }
        auto& entry = entries_.at(name);
        if (!entry->docs.Restore(std::move(loaded->documents))) {
            LOG(WARNING) << "skipping collection " << name << ": corrupt document snapshot";
            entries_.erase(name);
            continue;
        }
        if (entry->index != nullptr && !loaded->faiss_path.empty() &&
            !entry->index->load(loaded->faiss_path)) {
            LOG(WARNING) << "skipping collection " << name << ": failed to load vector index";
            entries_.erase(name);
            continue;
        }
        // 倒排索引不落盘：恢复时对快照文档重新分词重建。tombstone 已在
        // checkpoint 时压实，重建出的倒排不含任何死文档。
        entry->docs.ForEachActive(
            [&entry](const core::Document& doc) { entry->IndexText(doc.internal_docid, doc); });
        entry->docs.MarkCheckpointed();
        last_checkpoint_[name] = std::chrono::steady_clock::now();
        ++restored;
    }
    return restored;
}

bool CollectionRegistry::Checkpoint(const std::string& name) {
    if (store_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        return false;
    }
    if (!store_->Save(name,
                      ToProtoSpec(it->second->docs.schema(), name),
                      index_type_,
                      it->second->docs,
                      it->second->index.get())) {
        return false;
    }
    it->second->docs.MarkCheckpointed();
    last_checkpoint_[name] = std::chrono::steady_clock::now();
    return true;
}

void CollectionRegistry::CheckpointAll(bool force) {
    if (store_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& [name, entry] : entries_) {
        const size_t pending = entry->docs.PendingWrites();
        const auto last_it = last_checkpoint_.find(name);
        const auto last = last_it == last_checkpoint_.end() ? now : last_it->second;
        const bool due = force || pending >= kCheckpointWriteThreshold ||
                         (pending > 0 && now - last >= kCheckpointMaxAge);
        if (!due || pending == 0) {
            continue;
        }
        if (store_->Save(name,
                         ToProtoSpec(entry->docs.schema(), name),
                         index_type_,
                         entry->docs,
                         entry->index.get())) {
            entry->docs.MarkCheckpointed();
            last_checkpoint_[name] = now;
        }
    }
}

void CollectionRegistry::StartCheckpointLoop(std::shared_ptr<std::atomic<bool>> stop) {
    if (store_ == nullptr || worker_.joinable()) {
        return;
    }
    stop_ = std::move(stop);
    worker_ = std::thread([this]() {
        while (!stop_->load()) {
            CheckpointAll(/*force=*/false);
            for (int i = 0; i < 100 && !stop_->load(); ++i) {
                std::this_thread::sleep_for(kCheckpointInterval / 100);
            }
        }
        CheckpointAll(/*force=*/true); // final flush on shutdown
    });
}

CollectionRegistry::~CollectionRegistry() {
    if (worker_.joinable()) {
        if (stop_ != nullptr) {
            stop_->store(true);
        }
        worker_.join();
    }
    // store_ 是成员，在线程 join 之后才析构，无悬垂指针。
}

} // namespace pl::minisearch::server
