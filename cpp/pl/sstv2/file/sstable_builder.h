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
// Created: 2026/06/04 22:27

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/table_schema.h"
#include "cpp/pl/sstv2/types/variant.h"

namespace pl::sstv2::file {

struct SSTableBuilderOptions {
    size_t max_embedded_value_size = 1024;
    size_t max_data_block_size = 64 * 1024;
    size_t bloom_bits_per_key = 10;
    bool enable_bloom_filter = true;
    std::string value_file_path;
    bool emit_compatibility_metadata = true;
};

class SSTableBuilder {
public:
    using Options = SSTableBuilderOptions;

    SSTableBuilder(types::TableSchema schema, std::string_view key_file_path, Options opts = {});

    absl::Status add(const types::Row& row);

    // Compatibility helper for legacy tests/callers. It creates a single
    // String row key, Version=0, OpType=0, and String value.
    absl::Status add(std::string_view key, std::string_view value);

    absl::Status finish();
    void abort();

    [[nodiscard]] uint64_t total_rows() const;
    [[nodiscard]] uint64_t data_size() const;

private:
    struct PendingRow {
        types::Row row;
        std::string all_key;
        std::string value_bytes;
        std::string filename;
        uint64_t value_offset = 0;
        uint64_t value_length = 0;
        uint64_t value_checksum = 0;
        bool embedded = true;
    };

    absl::Status validate_schema() const;
    absl::Status validate_row(const types::Row& row) const;
    absl::Status append_value(types::Row row);
    absl::Status flush_data_block();

    [[nodiscard]] std::string encode_all_key(const types::Row& row) const;
    [[nodiscard]] std::string build_data_block(std::span<const PendingRow> rows) const;
    [[nodiscard]] std::string build_root_index_block() const;
    [[nodiscard]] std::string build_bloom_filter() const;
    [[nodiscard]] std::string build_metadata_section(
        uint32_t magic, const std::vector<std::pair<std::string, uint64_t>>& values) const;
    [[nodiscard]] std::string build_schema_metadata() const;

    types::TableSchema schema_;
    Options opts_;
    std::string key_file_path_;
    std::string key_file_;

    std::vector<PendingRow> pending_rows_;
    std::vector<std::pair<std::string, uint64_t>> value_file_entries_;
    std::vector<std::string> bloom_keys_;

    struct DataBlockIndex {
        std::string last_all_key;
        uint64_t offset = 0;
        uint64_t length = 0;
    };
    std::vector<DataBlockIndex> data_blocks_;

    uint64_t total_rows_ = 0;
    uint64_t data_size_ = 0;
    bool finished_ = false;
    bool aborted_ = false;
};

} // namespace pl::sstv2::file
