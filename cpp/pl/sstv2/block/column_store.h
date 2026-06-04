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
// Created: 2026/06/04 13:06

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/pattern/pattern_decoder.h"

namespace pl::sstv2::block {

struct SubColumnMeta {
    uint8_t pattern_id = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool has_bitmap = false;
    uint32_t bitmap_offset = 0;
    uint32_t bitmap_size = 0;
};

class ColumnStoreBuilder {
public:
    explicit ColumnStoreBuilder(size_t num_sub_columns);

    // Add a value for a sub-column at current row
    void add_value(size_t sub_col_idx, uint64_t value);

    // Mark a sub-column as null at current row
    void add_null(size_t sub_col_idx);

    // Must be called after adding all sub-column values for one row
    void finish_row();

    // Encode all sub-columns and produce the column store buffer
    absl::StatusOr<std::string> build();

    // Get metadata for all sub-columns (valid after build())
    const std::vector<SubColumnMeta>& sub_column_metas() const;

private:
    size_t num_sub_columns_;
    size_t num_rows_ = 0;
    std::vector<std::vector<uint64_t>> values_; // per sub-column
    std::vector<std::vector<bool>> null_flags_; // per sub-column, per row
    std::vector<SubColumnMeta> metas_;
};

class ColumnStoreReader {
public:
    ColumnStoreReader() = default;
    ColumnStoreReader(std::span<const std::byte> data,
                      std::span<const SubColumnMeta> metas,
                      size_t num_rows);

    // Get a value from a sub-column at a given row
    uint64_t get(size_t sub_col_idx, size_t row_idx) const;

    // Check if a value is null
    bool is_null(size_t sub_col_idx, size_t row_idx) const;

private:
    std::span<const std::byte> data_;
    std::vector<SubColumnMeta> metas_;
    size_t num_rows_ = 0;
    std::vector<std::unique_ptr<pl::sstv2::pattern::PatternDecoder>> decoders_;
};

} // namespace pl::sstv2::block
