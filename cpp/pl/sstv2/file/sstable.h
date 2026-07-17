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
// Created: 2026/06/06 14:16

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/bloom/bloom.h"
#include "cpp/pl/sstv2/compress/compress.h"
#include "cpp/pl/sstv2/format/metadata.h"
#include "cpp/pl/sstv2/index/index_tree.h"
#include "cpp/pl/sstv2/io/filesystem.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/row.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::file {

using KeyPrefix = types::KeyPrefix;

struct BuilderOptions {
    format::Configuration configuration;
    compress::Options block_compression;
    std::string value_file_name = "value.sstv2";
};

struct FinishResult {
    io::FileIdentity key_file;
    io::FileIdentity value_file;
    uint64_t row_count = 0;
    uint64_t sst_format_version = 1;
    uint64_t key_format_version = 0;
    uint64_t row_key_schema_fingerprint = 0;
    uint64_t comparator_domain_fingerprint = 0;
    uint64_t checksum_algorithm = 1;
    std::optional<std::string> min_key;
    std::optional<std::string> max_key;

    bool operator==(const FinishResult&) const = default;
};

struct Sinks {
    std::shared_ptr<io::FileSystem> filesystem;
    io::FileHandle key = io::kInvalidFileHandle;
    io::FileHandle value = io::kInvalidFileHandle;
};

struct ScanOptions {
    std::optional<KeyPrefix> start;
    std::optional<KeyPrefix> limit;
};

class Builder {
public:
    Builder(types::Schema::ConstRef schema, Sinks sinks, BuilderOptions options = {}) noexcept;
    ~Builder();

    [[nodiscard]] absl::Status add(const types::Row& row);
    [[nodiscard]] absl::StatusOr<FinishResult> finish_result();
    [[nodiscard]] static absl::StatusOr<FinishResult> finish_to_sinks(
        types::Schema::ConstRef schema,
        Sinks sinks,
        BuilderOptions options,
        const std::vector<types::Row>& rows);

private:
    [[nodiscard]] uint64_t max_data_block_rows() const noexcept;
    [[nodiscard]] uint64_t index_fanout() const noexcept;
    [[nodiscard]] block::Options data_block_options() const noexcept;
    [[nodiscard]] absl::StatusOr<size_t> encoded_data_block_size_with(
        const types::InternalRow& candidate, std::string_view candidate_embedded) const;
    [[nodiscard]] absl::Status flush_data_block();

    types::Schema::ConstRef schema_;
    types::InternalSchema::ConstRef internal_schema_;
    BuilderOptions options_;
    Sinks sinks_;
    std::unique_ptr<index::TreeBuilder> index_builder_;
    absl::Status initialization_status_;
    std::vector<types::InternalRow> pending_rows_;
    std::vector<std::string> pending_embedded_values_;
    bloom::Builder bloom_builder_;
    std::optional<types::AllKey> last_all_key_;
    uint64_t total_row_count_ = 0;
    uint64_t data_block_count_ = 0;
    std::optional<std::string> min_key_;
    std::optional<std::string> max_key_;
    enum class State : uint8_t { kOpen, kFinished, kFailed };
    State state_ = State::kOpen;
    std::optional<FinishResult> finish_result_;
};

class Reader {
public:
    class Iterator;
    struct ReaderState;

    [[nodiscard]] static absl::StatusOr<Reader> open(std::shared_ptr<io::FileSystem> filesystem,
                                                     io::FileHandle key_file,
                                                     io::FileHandle value_file);

    [[nodiscard]] types::Schema::ConstRef schema() const;
    [[nodiscard]] const format::Configuration& configuration() const;
    [[nodiscard]] const format::Statistics& statistics() const;
    [[nodiscard]] absl::StatusOr<std::vector<types::Row>> scan() const;
    [[nodiscard]] absl::StatusOr<std::vector<types::Row>> scan(const ScanOptions& options) const;
    [[nodiscard]] absl::StatusOr<std::optional<types::Row>> get(const types::AllKey& all_key) const;
    [[nodiscard]] absl::StatusOr<std::optional<types::Row>> get(const types::RowKey& row_key,
                                                                types::SystemKey system_key) const;
    [[nodiscard]] absl::StatusOr<Iterator> new_iterator(const ScanOptions& options = {}) const;

private:
    friend class Iterator;

    [[nodiscard]] absl::StatusOr<std::string> read_key_range(uint64_t offset,
                                                             uint64_t length) const;
    [[nodiscard]] absl::StatusOr<std::string> read_value_range(uint64_t offset,
                                                               uint64_t length) const;

    std::shared_ptr<const ReaderState> state_;
};

struct Reader::ReaderState {
    ~ReaderState();

    types::Schema::ConstRef schema;
    types::InternalSchema::ConstRef internal_schema;
    format::Configuration configuration;
    format::Statistics statistics;
    std::shared_ptr<io::FileSystem> filesystem;
    io::FileHandle key_file = io::kInvalidFileHandle;
    io::FileHandle value_file = io::kInvalidFileHandle;
    bloom::Reader bloom;
    index::BlockRef root;
};

class Reader::Iterator {
public:
    Iterator() = default;

    [[nodiscard]] absl::Status SeekToFirst();
    [[nodiscard]] absl::Status Seek(const KeyPrefix& target);
    [[nodiscard]] absl::Status Next();

    [[nodiscard]] bool Valid() const { return valid_ && status_.ok(); }
    [[nodiscard]] const types::Row& row() const { return current_row_; }
    [[nodiscard]] std::string_view key() const { return current_key_bytes_; }
    [[nodiscard]] const absl::Status& status() const { return status_; }
    [[nodiscard]] size_t state_slots_for_test() const;

private:
    friend class Reader;

    Iterator(std::shared_ptr<const ReaderState> state,
             std::optional<types::PrefixKey> start_key,
             std::optional<types::PrefixKey> limit_key);

    [[nodiscard]] absl::Status seek_impl(const std::optional<types::PrefixKey>& start_key);
    [[nodiscard]] absl::Status load_current_block(const std::optional<types::PrefixKey>& start_key,
                                                  bool apply_start_key);
    [[nodiscard]] absl::Status advance_to_next_valid();

    std::shared_ptr<const ReaderState> state_;
    std::optional<types::PrefixKey> start_key_;
    std::optional<types::PrefixKey> limit_key_;
    std::optional<index::ForwardCursor> cursor_;
    size_t row_index_ = 0;
    std::optional<block::BlockReader> block_;
    bool valid_ = false;
    types::Row current_row_;
    std::string current_key_bytes_;
    absl::Status status_;
};

} // namespace pl::sstv2::file
