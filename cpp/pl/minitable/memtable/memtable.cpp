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

#include "cpp/pl/minitable/memtable/memtable.h"

#include <cstring>
#include <limits>
#include <mutex>

namespace pl::minitable {

absl::StatusOr<std::shared_ptr<MemTable>> MemTable::Create(MemTableOptions options) {
    if (options.memory_limit_bytes == 0 || options.arena_block_bytes == 0 ||
        options.arena_block_bytes > options.memory_limit_bytes) {
        return absl::InvalidArgumentError("invalid MemTable memory configuration");
    }
    return std::shared_ptr<MemTable>(new MemTable(options));
}

MemTable::MemTable(MemTableOptions options)
    : options_(options), arena_(options.arena_block_bytes) {}

absl::StatusOr<std::string_view> MemTable::copy_to_arena(std::string_view bytes) {
    if (bytes.empty()) {
        return std::string_view{};
    }
    auto* destination = static_cast<char*>(arena_.allocate(bytes.size(), alignof(char)));
    if (destination == nullptr) {
        return absl::ResourceExhaustedError("MemTable arena allocation failed");
    }
    std::memcpy(destination, bytes.data(), bytes.size());
    return std::string_view(destination, bytes.size());
}

absl::Status MemTable::put(std::string_view encoded_key,
                           std::string_view encoded_value,
                           uint64_t apply_index) {
    if (encoded_key.empty()) {
        return absl::InvalidArgumentError("MemTable key is empty");
    }
    std::unique_lock lock(mutex_);
    if (frozen_) {
        return absl::FailedPreconditionError("MemTable is frozen");
    }
    if (apply_index < max_apply_index_) {
        return absl::InvalidArgumentError("apply index regressed");
    }
    const auto existing = entries_.find(encoded_key);
    const size_t key_bytes = existing == entries_.end() ? encoded_key.size() : size_t{0};
    const size_t remaining = options_.memory_limit_bytes -
                             std::min(charged_bytes_, options_.memory_limit_bytes);
    if (key_bytes > remaining || encoded_value.size() > remaining - key_bytes) {
        return absl::ResourceExhaustedError("MemTable memory limit exceeded");
    }
    const size_t incremental = key_bytes + encoded_value.size();

    if (existing != entries_.end()) {
        auto value = copy_to_arena(encoded_value);
        if (!value.ok()) {
            return value.status();
        }
        charged_bytes_ += incremental;
        existing->second = Entry{.value = *value, .apply_index = apply_index};
    } else {
        // Reserve a new entry's key and value in one allocation. If allocation fails,
        // the arena and ordered index remain unchanged as one logical operation.
        auto* storage = static_cast<char*>(arena_.allocate(incremental, alignof(char)));
        if (storage == nullptr) {
            return absl::ResourceExhaustedError("MemTable arena allocation failed");
        }
        std::memcpy(storage, encoded_key.data(), encoded_key.size());
        std::memcpy(storage + encoded_key.size(), encoded_value.data(), encoded_value.size());
        const std::string_view key(storage, encoded_key.size());
        const std::string_view value(storage + encoded_key.size(), encoded_value.size());
        charged_bytes_ += incremental;
        entries_.emplace(key, Entry{.value = value, .apply_index = apply_index});
    }
    max_apply_index_ = apply_index;
    return absl::OkStatus();
}

absl::Status MemTable::freeze() {
    std::unique_lock lock(mutex_);
    frozen_ = true;
    return absl::OkStatus();
}

bool MemTable::frozen() const {
    std::shared_lock lock(mutex_);
    return frozen_;
}

size_t MemTable::size() const {
    std::shared_lock lock(mutex_);
    return entries_.size();
}

size_t MemTable::memory_usage() const {
    std::shared_lock lock(mutex_);
    return arena_.get_stats().total_allocated + entries_.size() * sizeof(decltype(entries_)::value_type);
}

bool MemTable::should_flush() const {
    std::shared_lock lock(mutex_);
    return frozen_ || charged_bytes_ >= options_.memory_limit_bytes;
}

uint64_t MemTable::max_apply_index() const {
    std::shared_lock lock(mutex_);
    return max_apply_index_;
}

std::unique_ptr<sstv2::merge::ForwardCursor> MemTable::new_cursor() {
    return std::make_unique<Cursor>(shared_from_this());
}

MemTable::Cursor::Cursor(std::shared_ptr<MemTable> table) : table_(std::move(table)) {}

absl::Status MemTable::Cursor::position_at_or_after(std::string_view encoded_key, bool first) {
    std::shared_lock lock(table_->mutex_);
    const auto it = first ? table_->entries_.begin() : table_->entries_.lower_bound(encoded_key);
    positioned_ = true;
    valid_ = it != table_->entries_.end();
    current_key_ = valid_ ? it->first : std::string_view{};
    current_value_ = valid_ ? it->second.value : std::string_view{};
    return absl::OkStatus();
}

absl::Status MemTable::Cursor::seek_to_first() {
    return position_at_or_after({}, true);
}

absl::Status MemTable::Cursor::seek(std::string_view encoded_key) {
    return position_at_or_after(encoded_key, false);
}

absl::Status MemTable::Cursor::next() {
    if (!valid()) {
        return absl::FailedPreconditionError("MemTable cursor is not valid");
    }
    std::shared_lock lock(table_->mutex_);
    const auto it = table_->entries_.upper_bound(current_key_);
    valid_ = it != table_->entries_.end();
    current_key_ = valid_ ? it->first : std::string_view{};
    current_value_ = valid_ ? it->second.value : std::string_view{};
    return absl::OkStatus();
}

bool MemTable::Cursor::valid() const {
    return positioned_ && valid_;
}

std::string_view MemTable::Cursor::key() const {
    return valid() ? current_key_ : std::string_view{};
}

std::string_view MemTable::Cursor::value() const {
    return valid() ? current_value_ : std::string_view{};
}

} // namespace pl::minitable
