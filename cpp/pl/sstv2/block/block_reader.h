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
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/block/block_header.h"
#include "cpp/pl/sstv2/block/column_store.h"
#include "cpp/pl/sstv2/block/data_table.h"
#include "cpp/pl/sstv2/block/offset_table.h"
#include "cpp/pl/sstv2/block/rowkey_bitmap.h"

namespace pl::sstv2::block {

class BlockReader {
public:
    static absl::StatusOr<BlockReader> open(std::span<const std::byte> block_data,
                                            size_t num_sub_columns,
                                            size_t num_var_sub_columns);

    size_t num_rows() const;
    const BlockHeader& header() const;

    // Get a fixed sub-column value
    uint64_t get_fixed(size_t row_idx, size_t sub_col_idx) const;

    // Get a variable-length sub-column value
    std::string_view get_var(size_t row_idx, size_t var_col_idx) const;

    // Get row key
    std::string_view row_key(size_t row_idx) const;

    // Check if sub-column is null
    bool is_null(size_t row_idx, size_t sub_col_idx) const;

private:
    BlockReader() = default;

    BlockHeader header_;
    std::string decompressed_;
    size_t num_sub_columns_ = 0;
    size_t num_var_sub_columns_ = 0;

    DataTableReader data_table_reader_;
    ColumnStoreReader column_store_reader_;
    OffsetTableReader offset_table_reader_;
    RowKeyBitmapReader rowkey_bitmap_reader_;

    // Row key entries parsed from offset table at beginning
    std::vector<OffsetEntry> row_key_entries_;
    std::vector<SubColumnMeta> sub_column_metas_;
};

} // namespace pl::sstv2::block
