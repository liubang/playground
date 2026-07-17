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
// Created: 2026/07/18 00:36

#include "cpp/pl/minitable/core/slice_store.h"

#include <utility>

namespace pl::minitable {

absl::StatusOr<std::unique_ptr<SliceStore>> SliceStore::Create(
    std::map<uint32_t, MemTableOptions> locality_group_options) {
    if (locality_group_options.empty()) {
        return absl::InvalidArgumentError("SliceStore requires at least one locality group");
    }
    std::map<uint32_t, std::shared_ptr<MemTable>> locality_groups;
    for (const auto& [locality_group_id, options] : locality_group_options) {
        if (locality_group_id == 0) {
            return absl::InvalidArgumentError("locality group ID zero is reserved");
        }
        auto memtable = MemTable::Create(options);
        if (!memtable.ok()) {
            return memtable.status();
        }
        locality_groups.emplace(locality_group_id, std::move(*memtable));
    }
    return std::unique_ptr<SliceStore>(new SliceStore(std::move(locality_groups)));
}

absl::Status SliceStore::apply(std::span<const LocalityGroupPatch> patches,
                               uint64_t apply_index) {
    if (patches.empty()) {
        return absl::InvalidArgumentError("SliceStore patch list is empty");
    }

    std::lock_guard apply_lock(apply_mutex_);
    const uint64_t visible = visible_applied_index_.load(std::memory_order_relaxed);
    if (apply_index <= visible) {
        return absl::InvalidArgumentError("SliceStore apply index must advance");
    }

    std::map<uint32_t, std::span<const MemTableMutation>> canonical;
    for (const auto& patch : patches) {
        if (patch.locality_group_id == 0 || patch.mutations.empty()) {
            return absl::InvalidArgumentError("invalid locality group patch");
        }
        if (!locality_groups_.contains(patch.locality_group_id)) {
            return absl::NotFoundError("locality group does not exist");
        }
        if (!canonical.emplace(patch.locality_group_id, patch.mutations).second) {
            return absl::InvalidArgumentError("duplicate locality group patch");
        }
    }

    std::vector<MemTable::PreparedBatch> prepared;
    prepared.reserve(canonical.size());
    for (const auto& [locality_group_id, mutations] : canonical) {
        auto batch = locality_groups_.at(locality_group_id)->prepare_batch(mutations, apply_index);
        if (!batch.ok()) {
            return batch.status();
        }
        prepared.push_back(std::move(*batch));
    }

    // Publication is noexcept and allocation-free for every valid prepared token.
    for (auto& batch : prepared) {
        batch.publish();
    }
    visible_applied_index_.store(apply_index, std::memory_order_release);
    return absl::OkStatus();
}

SliceReadView SliceStore::read_view() const {
    const uint64_t visible = visible_applied_index_.load(std::memory_order_acquire);
    return SliceReadView(visible, locality_groups_);
}

absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> SliceReadView::new_cursor(
    uint32_t locality_group_id) const {
    const auto it = locality_groups_.find(locality_group_id);
    if (it == locality_groups_.end()) {
        return absl::NotFoundError("locality group does not exist in read view");
    }
    return it->second->new_cursor(visible_applied_index_);
}

} // namespace pl::minitable
