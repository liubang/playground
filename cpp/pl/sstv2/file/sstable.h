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

struct Files {
    std::string key_file;
    std::string value_file;
};

struct ScanOptions {
    std::optional<KeyPrefix> start;
    std::optional<KeyPrefix> limit;
};

class Builder {
public:
    Builder(types::Schema::ConstRef schema, BuilderOptions options = {});

    [[nodiscard]] absl::Status add(const types::Row& row);
    [[nodiscard]] absl::StatusOr<Files> finish();

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
    Files files_;
    std::unique_ptr<index::TreeBuilder> index_builder_;
    std::vector<types::InternalRow> pending_rows_;
    std::vector<std::string> pending_embedded_values_;
    bloom::Builder bloom_builder_;
    std::optional<types::AllKey> last_all_key_;
    uint64_t total_row_count_ = 0;
    uint64_t data_block_count_ = 0;
    bool finished_ = false;
};

class Reader {
public:
    [[nodiscard]] static absl::StatusOr<Reader> open(std::string_view key_file,
                                                     std::string_view value_file);

    [[nodiscard]] types::Schema::ConstRef schema() const { return schema_; }
    [[nodiscard]] const format::Configuration& configuration() const { return configuration_; }
    [[nodiscard]] const format::Statistics& statistics() const { return statistics_; }
    [[nodiscard]] absl::StatusOr<std::vector<types::Row>> scan() const;
    [[nodiscard]] absl::StatusOr<std::vector<types::Row>> scan(const ScanOptions& options) const;
    [[nodiscard]] absl::StatusOr<std::optional<types::Row>> get(
        const std::vector<types::Value>& key_columns,
        types::Version version,
        types::OpType op_type = types::OpType::kPut) const;

private:
    types::Schema::ConstRef schema_;
    types::InternalSchema::ConstRef internal_schema_;
    format::Configuration configuration_;
    format::Statistics statistics_;
    std::string key_file_;
    std::string value_file_;
    bloom::Reader bloom_;
    uint64_t root_index_offset_ = 0;
    uint64_t root_index_length_ = 0;
};

} // namespace pl::sstv2::file
