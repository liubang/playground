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
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/block/column_store.h"
#include "cpp/pl/sstv2/block/data_table.h"
#include "cpp/pl/sstv2/block/offset_table.h"
#include "cpp/pl/sstv2/block/rowkey_bitmap.h"
#include "cpp/pl/sstv2/compress/block_compressor.h"

namespace pl::sstv2::block {

class BlockWriter {
public:
    struct Options {
        size_t target_block_size = 64 * 1024;
        pl::sstv2::compress::CompressionType compression =
            pl::sstv2::compress::CompressionType::kNone;
    };

    explicit BlockWriter(size_t num_sub_columns, size_t num_var_sub_columns);
    BlockWriter(size_t num_sub_columns, size_t num_var_sub_columns, Options opts);

    // Add a row. Returns false if adding would exceed target block size.
    // fixed_values: uint64 values for fixed-size sub-columns
    // var_values: string data for variable-length sub-columns
    bool add_row(std::string_view row_key,
                 std::span<const uint64_t> fixed_values,
                 std::span<const std::string_view> var_values);

    absl::StatusOr<std::string> finish();
    bool empty() const;
    size_t num_rows() const;
    std::string_view first_row_key() const;
    std::string_view last_row_key() const;
    void reset();

private:
    Options opts_;
    size_t num_sub_columns_;
    size_t num_var_sub_columns_;

    ColumnStoreBuilder column_store_builder_;
    DataTableBuilder data_table_builder_;
    OffsetTableBuilder offset_table_builder_;
    RowKeyBitmapBuilder rowkey_bitmap_builder_;

    size_t num_rows_ = 0;
    size_t estimated_size_ = 0;
    std::string first_row_key_;
    std::string last_row_key_;
};

} // namespace pl::sstv2::block
