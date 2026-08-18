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
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/minisearch/core/document.h"
#include "cpp/pl/minisearch/core/id_mapper.h"
#include "cpp/pl/minisearch/core/schema.h"

namespace pl::minisearch::core {

// In-memory collection, M1 simplified form (DESIGN.md §5.1): active docstore
// keyed by internal docid, tombstones for deletes and superseded versions,
// collection-level shared_mutex (single writer, concurrent readers).
// The inverted and vector indexes plug in at the query layer; this class owns
// document identity and version convergence only.
class Collection {
public:
    Collection(std::string name, Schema schema);

    const std::string& name() const { return name_; }
    const Schema& schema() const { return schema_; }

    // Upsert semantics (DESIGN.md §4.2/§4.3):
    //  - unknown fields, wrong field types and mismatched vector dims are
    //    rejected before mutation;
    //  - version == 0 assigns a server-side monotonic version;
    //  - version > 0 must exceed the stored version (LWW), else rejected;
    //  - re-upsert allocates a new internal docid; the previous one is
    //    tombstoned and returned for index-layer invalidation.
    struct UpsertResult {
        bool ok = false;
        std::string error;
        int64_t internal_docid = -1;
        int64_t superseded_docid = -1; // -1 when the document is new
    };
    UpsertResult Upsert(Document doc);

    // Tombstones the active version. Returns false when the id is absent.
    bool Delete(const std::string& id);

    // Reads the latest version into `out`. Returns false when absent.
    bool Get(const std::string& id, Document* out) const;

    // Reads the active document with this internal docid. Returns false when
    // the docid is unknown or tombstoned (search result assembly path).
    bool GetByInternal(int64_t internal_docid, Document* out) const;

    size_t ActiveCount() const;

    // Copy of the current tombstone set (lock held internally). Returning a
    // snapshot keeps callers off the internal state without holding the lock.
    std::unordered_set<int64_t> TombstoneSnapshot() const;

    // Snapshot iteration over active documents (checkpoint path). The lock is
    // held for the duration of the callback.
    void ForEachActive(const std::function<void(const Document&)>& fn) const;

    // 活跃文档的分页快照（按 internal docid 升序，console 文档列表）。
    // total 返回活跃文档总数（不受分页影响）。
    std::vector<Document> ListDocuments(size_t offset, size_t limit, size_t* total) const;

    // Number of writes (upserts/deletes) since the last MarkCheckpointed.
    // Checkpoint scheduling input.
    size_t PendingWrites() const;

    // Resets the pending-write counter after a successful checkpoint. Writes
    // racing with a checkpoint keep the counter non-zero and are picked up by
    // the next one (DESIGN.md §5.1: no WAL in M0, minute-level window).
    void MarkCheckpointed();

    // Bulk restore from a checkpoint snapshot: rebuilds the id mapper and the
    // docid/version high-water marks. Assumes an empty collection. Empty
    // documents (active tombstones from the previous incarnation) must be
    // filtered by the caller.
    bool Restore(std::vector<Document> documents);

private:
    std::optional<std::string> ValidateAgainstSchema(const Document& doc) const;
    int64_t NextVersion() const;

    std::string name_;
    Schema schema_;
    mutable std::shared_mutex mu_;
    DocIdMapper mapper_;
    std::unordered_map<int64_t, Document> docs_; // active internal docid -> doc
    int64_t last_version_ = 0;
    size_t pending_writes_ = 0;
};

} // namespace pl::minisearch::core
