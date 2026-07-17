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
#include <map>
#include <memory>
#include <shared_mutex>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/arena/arena.h"
#include "cpp/pl/sstv2/merge/merge_iterator.h"

namespace pl::minitable {

struct MemTableOptions {
    size_t memory_limit_bytes = 64 * 1024 * 1024;
    size_t arena_block_bytes = 64 * 1024;
};

// Append-only ordered buffer. The replicated apply path is the sole writer;
// readers may run concurrently. freeze() is a one-way transition.
class MemTable final : public std::enable_shared_from_this<MemTable> {
public:
    class Cursor;

    [[nodiscard]] static absl::StatusOr<std::shared_ptr<MemTable>> Create(
        MemTableOptions options = {});

    [[nodiscard]] absl::Status put(std::string_view encoded_key,
                                   std::string_view encoded_value,
                                   uint64_t apply_index);
    [[nodiscard]] absl::Status freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t memory_usage() const;
    [[nodiscard]] bool should_flush() const;
    [[nodiscard]] uint64_t max_apply_index() const;
    [[nodiscard]] std::unique_ptr<sstv2::merge::ForwardCursor> new_cursor();

private:
    struct Entry {
        std::string_view value;
        uint64_t apply_index = 0;
    };
    struct KeyLess {
        using is_transparent = void;
        bool operator()(std::string_view lhs, std::string_view rhs) const { return lhs < rhs; }
    };

    explicit MemTable(MemTableOptions options);
    [[nodiscard]] absl::StatusOr<std::string_view> copy_to_arena(std::string_view bytes);

    MemTableOptions options_;
    mutable std::shared_mutex mutex_;
    Arena arena_;
    std::map<std::string_view, Entry, KeyLess> entries_;
    size_t charged_bytes_ = 0;
    uint64_t max_apply_index_ = 0;
    bool frozen_ = false;
};

class MemTable::Cursor final : public sstv2::merge::ForwardCursor {
public:
    explicit Cursor(std::shared_ptr<MemTable> table);

    [[nodiscard]] absl::Status seek_to_first() override;
    [[nodiscard]] absl::Status seek(std::string_view encoded_key) override;
    [[nodiscard]] absl::Status next() override;
    [[nodiscard]] bool valid() const override;
    [[nodiscard]] std::string_view key() const override;
    [[nodiscard]] std::string_view value() const override;

private:
    [[nodiscard]] absl::Status position_at_or_after(std::string_view encoded_key,
                                                    bool first);

    std::shared_ptr<MemTable> table_;
    std::string_view current_key_;
    std::string_view current_value_;
    bool positioned_ = false;
    bool valid_ = false;
};

} // namespace pl::minitable
