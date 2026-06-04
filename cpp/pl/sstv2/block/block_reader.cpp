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

#include "cpp/pl/sstv2/block/block_reader.h"

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/compress/block_compressor.h"
#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::block {

absl::StatusOr<BlockReader> BlockReader::open(std::span<const std::byte> block_data,
                                              size_t num_sub_columns,
                                              size_t num_var_sub_columns) {
    if (block_data.size() < BlockHeader::kSize) {
        return absl::InvalidArgumentError("block data too small for header");
    }

    // 1. Decode header
    auto header_result = BlockHeader::decode(block_data.data());
    if (!header_result.ok()) {
        return header_result.status();
    }

    BlockReader reader;
    reader.header_ = *header_result;
    reader.num_sub_columns_ = num_sub_columns;
    reader.num_var_sub_columns_ = num_var_sub_columns;

    // 2. Validate checksum
    auto payload_span = block_data.subspan(BlockHeader::kSize);
    std::string_view payload_sv(reinterpret_cast<const char*>(payload_span.data()),
                                payload_span.size());
    auto crc = absl::ComputeCrc32c(payload_sv);
    if (static_cast<uint32_t>(crc) != reader.header_.checksum) {
        return absl::DataLossError("block checksum mismatch");
    }

    // 3. Decompress if needed
    std::span<const std::byte> data_span;
    auto compression =
        static_cast<pl::sstv2::compress::CompressionType>(reader.header_.compression);
    if (compression != pl::sstv2::compress::CompressionType::kNone) {
        auto decompressed = pl::sstv2::compress::BlockCompressor::decompress(
            compression, payload_span, reader.header_.uncompressed_size);
        if (!decompressed.ok()) {
            return decompressed.status();
        }
        reader.decompressed_ = std::move(*decompressed);
        data_span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(reader.decompressed_.data()),
            reader.decompressed_.size());
    } else {
        data_span = payload_span;
    }

    // 4. Split payload into regions using sizes from header
    size_t dt_size = reader.header_.data_table_size;
    size_t cs_size = reader.header_.column_store_size;
    size_t ot_size = reader.header_.offset_table_size;
    size_t rk_size = data_span.size() - dt_size - cs_size - ot_size;

    auto dt_span = data_span.subspan(0, dt_size);
    auto cs_span = data_span.subspan(dt_size, cs_size);
    auto ot_span = data_span.subspan(dt_size + cs_size, ot_size);
    auto rk_span = data_span.subspan(dt_size + cs_size + ot_size, rk_size);

    // 5. Construct sub-readers
    reader.data_table_reader_ = DataTableReader(dt_span);

    size_t num_rows = reader.header_.num_rows;

    // Offset table layout per row: [row_key, var_0, var_1, ..., var_k]
    size_t entries_per_row = 1 + num_var_sub_columns;
    size_t total_entries = num_rows * entries_per_row;
    reader.offset_table_reader_ = OffsetTableReader(ot_span, total_entries);

    // Extract row key entries (first entry per row)
    reader.row_key_entries_.reserve(num_rows);
    for (size_t i = 0; i < num_rows; ++i) {
        reader.row_key_entries_.push_back(reader.offset_table_reader_.get(i * entries_per_row));
    }

    // Parse sub-column metas from column store header
    // Format per sub-column (18 bytes):
    //   pattern_id(1) + offset(4) + size(4) + has_bitmap(1) + bitmap_offset(4) + bitmap_size(4)
    using pl::sstv2::encode::decode_fixed;
    size_t meta_header_size = num_sub_columns * 18;
    reader.sub_column_metas_.resize(num_sub_columns);
    for (size_t i = 0; i < num_sub_columns; ++i) {
        const std::byte* p = cs_span.data() + i * 18;
        reader.sub_column_metas_[i].pattern_id = static_cast<uint8_t>(p[0]);
        reader.sub_column_metas_[i].offset =
            decode_fixed<uint32_t>(p + 1) + static_cast<uint32_t>(meta_header_size);
        reader.sub_column_metas_[i].size = decode_fixed<uint32_t>(p + 5);
        reader.sub_column_metas_[i].has_bitmap = static_cast<uint8_t>(p[9]) != 0;
        reader.sub_column_metas_[i].bitmap_offset =
            decode_fixed<uint32_t>(p + 10) + static_cast<uint32_t>(meta_header_size);
        reader.sub_column_metas_[i].bitmap_size = decode_fixed<uint32_t>(p + 14);
    }

    reader.column_store_reader_ = ColumnStoreReader(cs_span, reader.sub_column_metas_, num_rows);

    reader.rowkey_bitmap_reader_ = RowKeyBitmapReader(rk_span, num_rows);

    return reader;
}

size_t BlockReader::num_rows() const {
    return header_.num_rows;
}

const BlockHeader& BlockReader::header() const {
    return header_;
}

uint64_t BlockReader::get_fixed(size_t row_idx, size_t sub_col_idx) const {
    return column_store_reader_.get(sub_col_idx, row_idx);
}

std::string_view BlockReader::get_var(size_t row_idx, size_t var_col_idx) const {
    size_t entries_per_row = 1 + num_var_sub_columns_;
    size_t entry_idx = row_idx * entries_per_row + 1 + var_col_idx;
    auto entry = offset_table_reader_.get(entry_idx);
    return data_table_reader_.get(entry.offset, entry.length);
}

std::string_view BlockReader::row_key(size_t row_idx) const {
    auto entry = row_key_entries_[row_idx];
    return data_table_reader_.get(entry.offset, entry.length);
}

bool BlockReader::is_null(size_t row_idx, size_t sub_col_idx) const {
    return column_store_reader_.is_null(sub_col_idx, row_idx);
}

} // namespace pl::sstv2::block
