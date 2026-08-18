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

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pl::minisearch::core {

// Maps client-defined string ids to int64 internal docids with upsert
// reassignment semantics (DESIGN.md §4.3): re-upserting an id allocates a new
// internal docid and moves the previous one into the tombstone set; explicit
// deletes tombstone the active docid. Physical reclamation happens when the
// owning collection checkpoints.
class DocIdMapper {
public:
    // Returns the active internal docid for `id`, or -1 when absent.
    int64_t Lookup(const std::string& id) const;

    // Allocates a new internal docid for `id`. If `id` already exists, its
    // previous docid moves to the tombstone set and is returned via
    // `tombstoned` (nullopt otherwise).
    int64_t Assign(const std::string& id, std::optional<int64_t>* tombstoned);

    // Tombstones the active docid of `id`. Returns false when absent.
    bool Remove(const std::string& id);

    // Checkpoint restore: re-registers `id` at its persisted docid. Unlike
    // Assign this preserves the docid exactly — snapshot docids may be
    // sparse (deletes leave gaps) and FAISS frames are keyed by them.
    // Returns false when `id` is already mapped or docid < 0.
    bool RestoreMapping(const std::string& id, int64_t docid);

    const std::unordered_set<int64_t>& tombstones() const { return tombstones_; }

    size_t ActiveSize() const { return id_to_doc_.size(); }

    // Raises the internal docid counter to at least `floor` (checkpoint
    // restore: docids never get reused across incarnations).
    void AdvanceNext(int64_t floor) {
        if (next_ < floor) {
            next_ = floor;
        }
    }

private:
    int64_t next_ = 0;
    std::unordered_map<std::string, int64_t> id_to_doc_;
    std::unordered_set<int64_t> tombstones_;
};

} // namespace pl::minisearch::core
