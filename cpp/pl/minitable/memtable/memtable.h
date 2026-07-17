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
// Created: 2026/07/17 23:14

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/arena/arena.h"
#include "cpp/pl/sstv2/merge/merge_iterator.h"

namespace pl::minitable {

class SliceStore;

struct MemTableOptions {
    size_t memory_limit_bytes = 64 * 1024 * 1024;
    size_t arena_block_bytes = 64 * 1024;
};

struct MemTableMutation {
    std::string_view encoded_key;
    std::string_view encoded_value;
};

// Append-only ordered multi-version buffer. The replicated apply path is the
// sole writer; readers may run concurrently. freeze() is a one-way transition.
class MemTable final : public std::enable_shared_from_this<MemTable> {
public:
    class PreparedBatch;
    class Cursor;

    [[nodiscard]] static absl::StatusOr<std::shared_ptr<MemTable>> Create(
        MemTableOptions options = {});

    [[nodiscard]] absl::Status put(std::string_view encoded_key,
                                   std::string_view encoded_value,
                                   uint64_t apply_index);
    [[nodiscard]] absl::Status put_batch(std::span<const MemTableMutation> mutations,
                                         uint64_t apply_index);

    // Reserves and owns all memory needed by a batch without changing the ordered
    // index. Only one prepare may be outstanding per MemTable. Destroying an
    // unpublished token aborts it and rewinds its arena reservation.
    [[nodiscard]] absl::StatusOr<PreparedBatch> prepare_batch(
        std::span<const MemTableMutation> mutations, uint64_t apply_index);

    [[nodiscard]] absl::Status freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t memory_usage() const;
    [[nodiscard]] bool should_flush() const;
    [[nodiscard]] uint64_t max_apply_index() const;
    [[nodiscard]] std::unique_ptr<sstv2::merge::ForwardCursor> new_cursor(
        uint64_t read_visible_index = std::numeric_limits<uint64_t>::max());

private:
    friend class SliceStore;

    struct VersionNode {
        std::string_view value;
        uint64_t apply_index = 0;
        VersionNode* previous = nullptr;
    };
    struct KeyLess {
        using is_transparent = void;
        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
            return lhs < rhs;
        }
    };

    explicit MemTable(MemTableOptions options);

    MemTableOptions options_;
    mutable std::shared_mutex mutex_;
    Arena arena_;
    std::map<std::string_view, VersionNode*, KeyLess> entries_;
    size_t charged_bytes_ = 0;
    uint64_t max_apply_index_ = 0;
    bool frozen_ = false;
    bool prepare_pending_ = false;
};

class MemTable::PreparedBatch final {
public:
    PreparedBatch(const PreparedBatch&) = delete;
    PreparedBatch& operator=(const PreparedBatch&) = delete;
    PreparedBatch(PreparedBatch&& other) noexcept;
    PreparedBatch& operator=(PreparedBatch&&) = delete;
    ~PreparedBatch();

    // Commit is infallible after successful prepare. Violating this contract is
    // a programming error, not a recoverable storage status.
    void publish() noexcept;
    void abort() noexcept;
    [[nodiscard]] uint64_t apply_index() const { return apply_index_; }
    [[nodiscard]] bool pending() const { return table_ != nullptr; }

private:
    friend class MemTable;
    friend class SliceStore;
    using StagedEntries = std::map<std::string_view, VersionNode*, KeyLess>;

    PreparedBatch(std::shared_ptr<MemTable> table,
                  Arena::Checkpoint checkpoint,
                  StagedEntries new_entries,
                  std::vector<std::pair<VersionNode**, VersionNode*>> replacements,
                  size_t charged_bytes,
                  uint64_t apply_index);

    std::shared_ptr<MemTable> table_;
    Arena::Checkpoint checkpoint_;
    StagedEntries new_entries_;
    std::vector<std::pair<VersionNode**, VersionNode*>> replacements_;
    size_t charged_bytes_ = 0;
    uint64_t apply_index_ = 0;
};

class MemTable::Cursor final : public sstv2::merge::ForwardCursor {
public:
    Cursor(std::shared_ptr<MemTable> table, uint64_t read_visible_index);

    [[nodiscard]] absl::Status seek_to_first() override;
    [[nodiscard]] absl::Status seek(std::string_view encoded_key) override;
    [[nodiscard]] absl::Status next() override;
    [[nodiscard]] bool valid() const override;
    [[nodiscard]] std::string_view key() const override;
    [[nodiscard]] std::string_view value() const override;

private:
    using MapIterator = std::map<std::string_view, VersionNode*, KeyLess>::const_iterator;

    void position_from(MapIterator it);
    [[nodiscard]] absl::Status position_at_or_after(std::string_view encoded_key, bool first);

    std::shared_ptr<MemTable> table_;
    uint64_t read_visible_index_;
    std::string_view current_key_;
    std::string_view current_value_;
    bool positioned_ = false;
    bool valid_ = false;
};

} // namespace pl::minitable
