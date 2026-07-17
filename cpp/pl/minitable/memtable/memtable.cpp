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

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

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

absl::Status MemTable::put(std::string_view encoded_key,
                           std::string_view encoded_value,
                           uint64_t apply_index) {
    const MemTableMutation mutation{.encoded_key = encoded_key, .encoded_value = encoded_value};
    return put_batch(std::span<const MemTableMutation>(&mutation, 1), apply_index);
}

absl::Status MemTable::put_batch(std::span<const MemTableMutation> mutations,
                                 uint64_t apply_index) {
    auto prepared = prepare_batch(mutations, apply_index);
    if (!prepared.ok()) {
        return prepared.status();
    }
    prepared->publish();
    return absl::OkStatus();
}

absl::StatusOr<MemTable::PreparedBatch> MemTable::prepare_batch(
    std::span<const MemTableMutation> mutations, uint64_t apply_index) {
    if (mutations.empty()) {
        return absl::InvalidArgumentError("MemTable mutation batch is empty");
    }

    std::map<std::string_view, std::string_view, KeyLess> canonical;
    try {
        for (const auto& mutation : mutations) {
            if (mutation.encoded_key.empty()) {
                return absl::InvalidArgumentError("MemTable key is empty");
            }
            canonical.insert_or_assign(mutation.encoded_key, mutation.encoded_value);
        }
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("MemTable batch canonicalization allocation failed");
    }

    std::unique_lock lock(mutex_);
    if (frozen_) {
        return absl::FailedPreconditionError("MemTable is frozen");
    }
    if (prepare_pending_) {
        return absl::FailedPreconditionError("MemTable already has a prepared batch");
    }
    if (apply_index == 0 || apply_index <= max_apply_index_) {
        return absl::InvalidArgumentError("apply index must strictly advance");
    }

    size_t incremental = 0;
    const size_t remaining = options_.memory_limit_bytes -
                             std::min(charged_bytes_, options_.memory_limit_bytes);
    for (const auto& [key, value] : canonical) {
        const size_t key_bytes = entries_.contains(key) ? size_t{0} : key.size();
        if (key_bytes > remaining - incremental ||
            value.size() > remaining - incremental - key_bytes) {
            return absl::ResourceExhaustedError("MemTable memory limit exceeded");
        }
        incremental += key_bytes + value.size();
        const size_t aligned =
            (incremental + alignof(VersionNode) - 1) & ~(alignof(VersionNode) - 1);
        if (aligned < incremental || aligned > remaining ||
            sizeof(VersionNode) > remaining - aligned) {
            return absl::ResourceExhaustedError("MemTable memory limit exceeded");
        }
        incremental = aligned + sizeof(VersionNode);
    }

    const auto checkpoint = arena_.checkpoint();
    try {
        auto* storage = static_cast<char*>(arena_.allocate(incremental, alignof(VersionNode)));
        if (storage == nullptr) {
            static_cast<void>(arena_.rewind(checkpoint));
            return absl::ResourceExhaustedError("MemTable arena allocation failed");
        }

        PreparedBatch::StagedEntries new_entries;
        std::vector<std::pair<VersionNode**, VersionNode*>> replacements;
        replacements.reserve(canonical.size());

        size_t offset = 0;
        for (const auto& [key, value] : canonical) {
            auto existing = entries_.find(key);
            std::string_view owned_key;
            if (existing == entries_.end()) {
                std::memcpy(storage + offset, key.data(), key.size());
                owned_key = std::string_view(storage + offset, key.size());
                offset += key.size();
            } else {
                owned_key = existing->first;
            }

            std::string_view owned_value;
            if (!value.empty()) {
                std::memcpy(storage + offset, value.data(), value.size());
                owned_value = std::string_view(storage + offset, value.size());
                offset += value.size();
            }
            const size_t aligned_offset =
                (offset + alignof(VersionNode) - 1) & ~(alignof(VersionNode) - 1);
            auto* node = new (storage + aligned_offset)
                VersionNode{.value = owned_value,
                            .apply_index = apply_index,
                            .previous = existing == entries_.end() ? nullptr : existing->second};
            offset = aligned_offset + sizeof(VersionNode);

            if (existing == entries_.end()) {
                new_entries.emplace(owned_key, node);
            } else {
                replacements.emplace_back(&existing->second, node);
            }
        }

        auto table = shared_from_this();
        prepare_pending_ = true;
        return PreparedBatch(std::move(table),
                             checkpoint,
                             std::move(new_entries),
                             std::move(replacements),
                             incremental,
                             apply_index);
    } catch (const std::bad_alloc&) {
        static_cast<void>(arena_.rewind(checkpoint));
        return absl::ResourceExhaustedError("MemTable batch preparation allocation failed");
    } catch (...) {
        static_cast<void>(arena_.rewind(checkpoint));
        throw;
    }
}

MemTable::PreparedBatch::PreparedBatch(
    std::shared_ptr<MemTable> table,
    Arena::Checkpoint checkpoint,
    StagedEntries new_entries,
    std::vector<std::pair<VersionNode**, VersionNode*>> replacements,
    size_t charged_bytes,
    uint64_t apply_index)
    : table_(std::move(table)),
      checkpoint_(checkpoint),
      new_entries_(std::move(new_entries)),
      replacements_(std::move(replacements)),
      charged_bytes_(charged_bytes),
      apply_index_(apply_index) {}

MemTable::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept
    : table_(std::move(other.table_)),
      checkpoint_(other.checkpoint_),
      new_entries_(std::move(other.new_entries_)),
      replacements_(std::move(other.replacements_)),
      charged_bytes_(other.charged_bytes_),
      apply_index_(other.apply_index_) {}

MemTable::PreparedBatch::~PreparedBatch() {
    abort();
}

void MemTable::PreparedBatch::publish() noexcept {
    if (table_ == nullptr) {
        std::terminate();
    }
    std::unique_lock lock(table_->mutex_);
    for (const auto& [slot, node] : replacements_) {
        *slot = node;
    }
    table_->entries_.merge(new_entries_);
    table_->charged_bytes_ += charged_bytes_;
    table_->max_apply_index_ = apply_index_;
    table_->prepare_pending_ = false;
    if (!table_->arena_.commit(checkpoint_)) {
        std::terminate();
    }
    auto table = std::move(table_);
    lock.unlock();
    table.reset();
}

void MemTable::PreparedBatch::abort() noexcept {
    if (table_ == nullptr) {
        return;
    }
    std::unique_lock lock(table_->mutex_);
    if (!table_->arena_.rewind(checkpoint_)) {
        std::terminate();
    }
    table_->prepare_pending_ = false;
    auto table = std::move(table_);
    lock.unlock();
    table.reset();
}

absl::Status MemTable::freeze() {
    std::unique_lock lock(mutex_);
    if (prepare_pending_) {
        return absl::FailedPreconditionError("MemTable has a prepared batch");
    }
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
    return arena_.get_stats().total_allocated +
           entries_.size() * sizeof(decltype(entries_)::value_type);
}

bool MemTable::should_flush() const {
    std::shared_lock lock(mutex_);
    return frozen_ || charged_bytes_ >= options_.memory_limit_bytes;
}

uint64_t MemTable::max_apply_index() const {
    std::shared_lock lock(mutex_);
    return max_apply_index_;
}

std::unique_ptr<sstv2::merge::ForwardCursor> MemTable::new_cursor(
    uint64_t read_visible_index) {
    return std::make_unique<Cursor>(shared_from_this(), read_visible_index);
}

MemTable::Cursor::Cursor(std::shared_ptr<MemTable> table, uint64_t read_visible_index)
    : table_(std::move(table)), read_visible_index_(read_visible_index) {}

void MemTable::Cursor::position_from(MapIterator it) {
    while (it != table_->entries_.end()) {
        auto* version = it->second;
        while (version != nullptr && version->apply_index > read_visible_index_) {
            version = version->previous;
        }
        if (version != nullptr) {
            valid_ = true;
            current_key_ = it->first;
            current_value_ = version->value;
            return;
        }
        ++it;
    }
    valid_ = false;
    current_key_ = {};
    current_value_ = {};
}

absl::Status MemTable::Cursor::position_at_or_after(std::string_view encoded_key, bool first) {
    std::shared_lock lock(table_->mutex_);
    const auto it = first ? table_->entries_.begin() : table_->entries_.lower_bound(encoded_key);
    positioned_ = true;
    position_from(it);
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
    position_from(table_->entries_.upper_bound(current_key_));
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
