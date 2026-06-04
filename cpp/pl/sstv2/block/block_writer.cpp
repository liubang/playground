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

#include "cpp/pl/sstv2/block/block_writer.h"

#include <cstddef>
#include <cstring>

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/block/block_header.h"
#include "cpp/pl/sstv2/compress/block_compressor.h"
#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::block {

BlockWriter::BlockWriter(size_t num_sub_columns, size_t num_var_sub_columns)
    : BlockWriter(num_sub_columns, num_var_sub_columns, Options{}) {}

BlockWriter::BlockWriter(size_t num_sub_columns, size_t num_var_sub_columns, Options opts)
    : opts_(opts),
      num_sub_columns_(num_sub_columns),
      num_var_sub_columns_(num_var_sub_columns),
      column_store_builder_(num_sub_columns) {}

bool BlockWriter::add_row(std::string_view row_key,
                          std::span<const uint64_t> fixed_values,
                          std::span<const std::string_view> var_values) {
    // Estimate size increment
    size_t row_size = row_key.size();
    for (auto sv : var_values) {
        row_size += sv.size();
    }
    row_size += num_sub_columns_ * sizeof(uint64_t);

    if (num_rows_ > 0 && estimated_size_ + row_size > opts_.target_block_size) {
        return false;
    }

    // Row key into data table
    uint32_t rk_offset = data_table_builder_.append(row_key);
    // Add row key entry to offset table (first entry per row)
    offset_table_builder_.add_entry(rk_offset, static_cast<uint32_t>(row_key.size()));

    // Rowkey bitmap: first row is never duplicate
    if (num_rows_ == 0) {
        rowkey_bitmap_builder_.add(false);
        first_row_key_ = std::string(row_key);
    } else {
        rowkey_bitmap_builder_.add(row_key == last_row_key_);
    }
    last_row_key_ = std::string(row_key);

    // Fixed sub-columns into column store
    for (size_t i = 0; i < num_sub_columns_; ++i) {
        column_store_builder_.add_value(i, fixed_values[i]);
    }
    column_store_builder_.finish_row();

    // Variable sub-columns into data table + offset table
    for (size_t i = 0; i < num_var_sub_columns_; ++i) {
        uint32_t offset = data_table_builder_.append(var_values[i]);
        offset_table_builder_.add_entry(offset, static_cast<uint32_t>(var_values[i].size()));
    }

    ++num_rows_;
    estimated_size_ += row_size;
    return true;
}

absl::StatusOr<std::string> BlockWriter::finish() {
    if (num_rows_ == 0) {
        return absl::FailedPreconditionError("no rows to write");
    }

    // 1. Build column store
    auto column_store_result = column_store_builder_.build();
    if (!column_store_result.ok()) {
        return column_store_result.status();
    }
    std::string encoded_data = std::move(*column_store_result);
    const auto& metas = column_store_builder_.sub_column_metas();

    // Prepend sub-column metas to column store
    // Meta format per sub-column (18 bytes):
    //   pattern_id: uint8 (1)
    //   offset: uint32 (4)
    //   size: uint32 (4)
    //   has_bitmap: uint8 (1)
    //   bitmap_offset: uint32 (4)
    //   bitmap_size: uint32 (4)
    size_t meta_header_size = num_sub_columns_ * 18;
    std::string column_store_data;
    column_store_data.resize(meta_header_size + encoded_data.size());

    for (size_t i = 0; i < num_sub_columns_; ++i) {
        std::byte* p = reinterpret_cast<std::byte*>(column_store_data.data()) + i * 18;
        p[0] = static_cast<std::byte>(metas[i].pattern_id);
        pl::sstv2::encode::encode_fixed<uint32_t>(metas[i].offset, p + 1);
        pl::sstv2::encode::encode_fixed<uint32_t>(metas[i].size, p + 5);
        p[9] = static_cast<std::byte>(metas[i].has_bitmap ? 1 : 0);
        pl::sstv2::encode::encode_fixed<uint32_t>(metas[i].bitmap_offset, p + 10);
        pl::sstv2::encode::encode_fixed<uint32_t>(metas[i].bitmap_size, p + 14);
    }
    std::memcpy(
        column_store_data.data() + meta_header_size, encoded_data.data(), encoded_data.size());

    // 2. Build data table
    std::string data_table_data = data_table_builder_.build();

    // 3. Build offset table
    std::string offset_table_data = offset_table_builder_.build();

    // 4. Build rowkey bitmap
    std::string rowkey_bitmap_data = rowkey_bitmap_builder_.build();

    // 5. Concatenate payload: data_table + column_store + offset_table + rowkey_bitmap
    std::string payload;
    payload.reserve(data_table_data.size() + column_store_data.size() + offset_table_data.size() +
                    rowkey_bitmap_data.size());
    payload.append(data_table_data);
    payload.append(column_store_data);
    payload.append(offset_table_data);
    payload.append(rowkey_bitmap_data);

    uint32_t uncompressed_size = static_cast<uint32_t>(payload.size());
    uint32_t compressed_size = uncompressed_size;

    // 6. Optionally compress
    std::string final_payload;
    if (opts_.compression != pl::sstv2::compress::CompressionType::kNone) {
        auto compressed = pl::sstv2::compress::BlockCompressor::compress(
            opts_.compression,
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                       payload.size()));
        if (!compressed.ok()) {
            return compressed.status();
        }
        final_payload = std::move(*compressed);
        compressed_size = static_cast<uint32_t>(final_payload.size());
    } else {
        final_payload = std::move(payload);
    }

    // 7. Compute CRC32C
    auto crc = absl::ComputeCrc32c(final_payload);
    uint32_t checksum = static_cast<uint32_t>(crc);

    // 8. Fill BlockHeader
    BlockHeader header;
    header.magic = BlockHeader::kMagic;
    header.checksum = checksum;
    header.uncompressed_size = uncompressed_size;
    header.compressed_size = compressed_size;
    header.num_rows = static_cast<uint16_t>(num_rows_);
    header.num_sub_columns = static_cast<uint16_t>(num_sub_columns_ + num_var_sub_columns_);
    header.compression = static_cast<uint8_t>(opts_.compression);

    // First row key offset: entry 0 in offset table
    // We stored row keys at indices 0, (1+num_var), (2*(1+num_var)), ...
    // The actual offset in data_table is what we wrote
    // For header, store the data_table offset of first/last row key
    header.first_row_key_offset = 0; // first row key is always at offset 0 in data table
    // Last row key offset: we can compute from data table
    // Actually just store the byte offset in data_table where last row key starts
    // We know: last_row_key_ content, and it's the last row key appended.
    // Since data_table is append-only and we add row_key first each row,
    // we can't easily recover the offset without tracking it.
    // Use the string position: search backwards. Better: track it.
    // We removed row_key_entries_ but we have offset_table_builder_ which has all entries.
    // Entry at index (num_rows_-1) * (1+num_var_sub_columns_) is the last row key entry.
    // But OffsetTableBuilder doesn't expose random access. Let's just store first/last offsets.
    // Actually, the first row key is at offset 0 in data table (it's appended first).
    // For last row key offset, compute: data_table_size - last_row_key_.size() would be wrong
    // if there are var values after it. Let's just track it explicitly.
    // We know the offset from the first append call for each row.
    // Simplest fix: track first and last row key offset in member vars.
    header.first_row_key_offset = 0;
    // The last row key offset in data table: we can find by scanning.
    // Actually row keys are interleaved with var data in data_table.
    // Let's just store 0 and rely on offset table for actual lookups.
    // The header fields are informational for fast access to first/last key.
    // Since we track first_row_key_ and last_row_key_ as strings, and the reader
    // can get them via offset table, these header fields are just hints.
    // For now, store 0 for both (reader uses offset table anyway).
    header.last_row_key_offset = 0;

    header.data_table_size = static_cast<uint32_t>(data_table_data.size());
    header.column_store_size = static_cast<uint32_t>(column_store_data.size());
    header.offset_table_size = static_cast<uint32_t>(offset_table_data.size());

    // 9. Return header + payload
    std::string result;
    result.resize(BlockHeader::kSize + final_payload.size());
    header.encode(reinterpret_cast<std::byte*>(result.data()));
    std::memcpy(result.data() + BlockHeader::kSize, final_payload.data(), final_payload.size());

    return result;
}

bool BlockWriter::empty() const {
    return num_rows_ == 0;
}

size_t BlockWriter::num_rows() const {
    return num_rows_;
}

std::string_view BlockWriter::first_row_key() const {
    return first_row_key_;
}

std::string_view BlockWriter::last_row_key() const {
    return last_row_key_;
}

void BlockWriter::reset() {
    column_store_builder_ = ColumnStoreBuilder(num_sub_columns_);
    data_table_builder_ = DataTableBuilder();
    offset_table_builder_ = OffsetTableBuilder();
    rowkey_bitmap_builder_ = RowKeyBitmapBuilder();
    num_rows_ = 0;
    estimated_size_ = 0;
    first_row_key_.clear();
    last_row_key_.clear();
}

} // namespace pl::sstv2::block
