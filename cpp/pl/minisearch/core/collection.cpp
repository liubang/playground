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

#include "cpp/pl/minisearch/core/collection.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_set>

namespace pl::minisearch::core {

Collection::Collection(std::string name, Schema schema)
    : name_(std::move(name)), schema_(std::move(schema)) {}

std::optional<std::string> Collection::ValidateAgainstSchema(const Document& doc) const {
    for (const auto& [field, value] : doc.fields) {
        const FieldDef* def = schema_.Find(field);
        if (def == nullptr) {
            return "unknown field: " + field;
        }
        const bool holds_string = std::holds_alternative<std::string>(value);
        const bool holds_double = std::holds_alternative<double>(value);
        const bool holds_vector = std::holds_alternative<std::vector<float>>(value);
        switch (def->type) {
            case FieldType::kText:
            case FieldType::kKeyword:
                if (!holds_string) {
                    return "field expects string value: " + field;
                }
                break;
            case FieldType::kNumeric:
                if (!holds_double) {
                    return "field expects numeric value: " + field;
                }
                break;
            case FieldType::kVector:
                if (!holds_vector) {
                    return "field expects vector value: " + field;
                }
                if (static_cast<int>(std::get<std::vector<float>>(value).size()) != def->dims) {
                    return "vector dims mismatch for field " + field + " (expected " +
                           std::to_string(def->dims) + ")";
                }
                break;
        }
    }
    return std::nullopt;
}

int64_t Collection::NextVersion() const {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return now > last_version_ ? now : last_version_ + 1;
}

Collection::UpsertResult Collection::Upsert(Document doc) {
    UpsertResult result;
    if (doc.id.empty()) {
        result.error = "document id must not be empty";
        return result;
    }
    if (auto err = ValidateAgainstSchema(doc); err.has_value()) {
        result.error = *err;
        return result;
    }

    std::unique_lock<std::shared_mutex> lock(mu_);
    const int64_t active = mapper_.Lookup(doc.id);
    if (active >= 0) {
        const int64_t stored_version = docs_.at(active).version;
        if (doc.version > 0 && doc.version <= stored_version) {
            result.error = "stale version " + std::to_string(doc.version) + " (stored " +
                           std::to_string(stored_version) + ")";
            return result;
        }
    }
    if (doc.version == 0) {
        doc.version = NextVersion();
    }

    std::optional<int64_t> tombstoned;
    const int64_t fresh = mapper_.Assign(doc.id, &tombstoned);
    doc.internal_docid = fresh;
    docs_.emplace(fresh, std::move(doc));
    last_version_ = std::max(last_version_, docs_.at(fresh).version);
    ++pending_writes_;

    result.ok = true;
    result.internal_docid = fresh;
    result.superseded_docid = tombstoned.value_or(-1);
    return result;
}

bool Collection::Delete(const std::string& id) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!mapper_.Remove(id)) {
        return false;
    }
    // The tombstoned docid stays in docs_ until checkpoint compaction; Get
    // resolves through the mapper, so it is unreachable from now on.
    ++pending_writes_;
    return true;
}

bool Collection::Get(const std::string& id, Document* out) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    const int64_t active = mapper_.Lookup(id);
    if (active < 0) {
        return false;
    }
    *out = docs_.at(active);
    return true;
}

bool Collection::GetByInternal(int64_t internal_docid, Document* out) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = docs_.find(internal_docid);
    if (it == docs_.end() || mapper_.tombstones().count(internal_docid) > 0) {
        return false;
    }
    *out = it->second;
    return true;
}

size_t Collection::ActiveCount() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return docs_.size() - mapper_.tombstones().size();
}

std::unordered_set<int64_t> Collection::TombstoneSnapshot() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return mapper_.tombstones();
}

void Collection::ForEachActive(const std::function<void(const Document&)>& fn) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    for (const auto& [docid, doc] : docs_) {
        if (mapper_.tombstones().count(docid) == 0) {
            fn(doc);
        }
    }
}

std::vector<Document> Collection::ListDocuments(size_t offset, size_t limit, size_t* total) const {
    std::vector<Document> all;
    ForEachActive([&](const Document& doc) { all.push_back(doc); });
    std::sort(all.begin(), all.end(), [](const Document& a, const Document& b) {
        return a.internal_docid < b.internal_docid;
    });
    *total = all.size();
    if (offset >= all.size()) {
        return {};
    }
    const size_t end = std::min(all.size(), offset + limit);
    return {all.begin() + static_cast<std::ptrdiff_t>(offset),
            all.begin() + static_cast<std::ptrdiff_t>(end)};
}

size_t Collection::PendingWrites() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return pending_writes_;
}

void Collection::MarkCheckpointed() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    pending_writes_ = 0;
}

bool Collection::Restore(std::vector<Document> documents) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!docs_.empty() || mapper_.ActiveSize() > 0) {
        return false;
    }
    // 快照 docid 可稀疏（删除留洞）且帧序任意（写入侧按 docid 排序仅为
    // 输出确定）；按原值恢复映射，重复 id/docid 视为损坏。
    std::unordered_set<int64_t> seen_docids;
    for (auto& doc : documents) {
        if (doc.id.empty() || !mapper_.RestoreMapping(doc.id, doc.internal_docid) ||
            !seen_docids.insert(doc.internal_docid).second) {
            return false;
        }
        last_version_ = std::max(last_version_, doc.version);
        docs_.emplace(doc.internal_docid, std::move(doc));
    }
    pending_writes_ = 0;
    return true;
}

} // namespace pl::minisearch::core
