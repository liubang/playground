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

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minitable/memtable/memtable.h"

namespace pl::minitable {

struct LocalityGroupPatch {
    uint32_t locality_group_id = 0;
    std::span<const MemTableMutation> mutations;
};

class SliceReadView final {
public:
    [[nodiscard]] uint64_t visible_applied_index() const { return visible_applied_index_; }
    [[nodiscard]] absl::StatusOr<std::unique_ptr<sstv2::merge::ForwardCursor>> new_cursor(
        uint32_t locality_group_id) const;

private:
    friend class SliceStore;
    SliceReadView(uint64_t visible_applied_index,
                  std::map<uint32_t, std::shared_ptr<MemTable>> locality_groups)
        : visible_applied_index_(visible_applied_index),
          locality_groups_(std::move(locality_groups)) {}

    uint64_t visible_applied_index_ = 0;
    std::map<uint32_t, std::shared_ptr<MemTable>> locality_groups_;
};

// Minimal in-process Slice apply coordinator. It prepares all LG MemTables in
// stable ID order, performs infallible publication, then release-publishes one
// Slice-wide visibility watermark for readers to acquire and pin.
class SliceStore final {
public:
    // SliceStore constructs and exclusively owns every writable MemTable. Read
    // views expose cursors only, so no caller can bypass the Slice watermark.
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<SliceStore>> Create(
        std::map<uint32_t, MemTableOptions> locality_groups);

    [[nodiscard]] absl::Status apply(std::span<const LocalityGroupPatch> patches,
                                     uint64_t apply_index);
    [[nodiscard]] SliceReadView read_view() const;
    [[nodiscard]] uint64_t visible_applied_index() const {
        return visible_applied_index_.load(std::memory_order_acquire);
    }

private:
    explicit SliceStore(std::map<uint32_t, std::shared_ptr<MemTable>> locality_groups)
        : locality_groups_(std::move(locality_groups)) {}

    // Raft apply has one writer. This mutex also makes accidental concurrent
    // callers obey the same deterministic order.
    mutable std::mutex apply_mutex_;
    std::map<uint32_t, std::shared_ptr<MemTable>> locality_groups_;
    std::atomic<uint64_t> visible_applied_index_{0};
};

} // namespace pl::minitable
