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

#include "cpp/pl/sstv2/block/column_store.h"

#include "cpp/pl/sstv2/pattern/pattern_decoder.h"
#include "cpp/pl/sstv2/pattern/pattern_selector.h"

namespace pl::sstv2::block {

ColumnStoreBuilder::ColumnStoreBuilder(size_t num_sub_columns)
    : num_sub_columns_(num_sub_columns), values_(num_sub_columns), null_flags_(num_sub_columns) {}

void ColumnStoreBuilder::add_value(size_t sub_col_idx, uint64_t value) {
    values_[sub_col_idx].push_back(value);
    null_flags_[sub_col_idx].push_back(false);
}

void ColumnStoreBuilder::add_null(size_t sub_col_idx) {
    values_[sub_col_idx].push_back(0);
    null_flags_[sub_col_idx].push_back(true);
}

void ColumnStoreBuilder::finish_row() {
    ++num_rows_;
}

absl::StatusOr<std::string> ColumnStoreBuilder::build() {
    metas_.clear();
    metas_.resize(num_sub_columns_);
    std::string output;

    for (size_t i = 0; i < num_sub_columns_; ++i) {
        auto& meta = metas_[i];
        meta.offset = static_cast<uint32_t>(output.size());

        // Check if there are any nulls
        bool has_nulls = false;
        for (bool flag : null_flags_[i]) {
            if (flag) {
                has_nulls = true;
                break;
            }
        }

        // Select pattern and encode
        auto selection = pl::sstv2::pattern::PatternSelector::select(values_[i]);
        meta.pattern_id = static_cast<uint8_t>(selection.pattern_id);

        std::string encoded;
        auto status = selection.encoder->encode(values_[i], encoded);
        if (!status.ok()) {
            return status;
        }
        output.append(encoded);
        meta.size = static_cast<uint32_t>(encoded.size());

        // Append null bitmap if needed
        meta.has_bitmap = has_nulls;
        if (has_nulls) {
            meta.bitmap_offset = static_cast<uint32_t>(output.size());
            size_t bitmap_bytes = (num_rows_ + 7) / 8;
            std::string bitmap(bitmap_bytes, '\0');
            for (size_t row = 0; row < num_rows_; ++row) {
                if (null_flags_[i][row]) {
                    bitmap[row / 8] |= static_cast<char>(1 << (row % 8));
                }
            }
            output.append(bitmap);
            meta.bitmap_size = static_cast<uint32_t>(bitmap_bytes);
        } else {
            meta.bitmap_offset = 0;
            meta.bitmap_size = 0;
        }
    }

    return output;
}

const std::vector<SubColumnMeta>& ColumnStoreBuilder::sub_column_metas() const {
    return metas_;
}

ColumnStoreReader::ColumnStoreReader(std::span<const std::byte> data,
                                     std::span<const SubColumnMeta> metas,
                                     size_t num_rows)
    : data_(data), metas_(metas.begin(), metas.end()), num_rows_(num_rows) {
    decoders_.reserve(metas_.size());
    for (const auto& meta : metas_) {
        auto decoder = pl::sstv2::pattern::PatternDecoder::create(
            static_cast<pl::sstv2::pattern::PatternId>(meta.pattern_id),
            data_.subspan(meta.offset, meta.size),
            num_rows_);
        decoders_.push_back(std::move(decoder));
    }
}

uint64_t ColumnStoreReader::get(size_t sub_col_idx, size_t row_idx) const {
    return decoders_[sub_col_idx]->get(row_idx);
}

bool ColumnStoreReader::is_null(size_t sub_col_idx, size_t row_idx) const {
    const auto& meta = metas_[sub_col_idx];
    if (!meta.has_bitmap) {
        return false;
    }
    const auto* bitmap = data_.data() + meta.bitmap_offset;
    auto byte_val = static_cast<uint8_t>(bitmap[row_idx / 8]);
    return (byte_val >> (row_idx % 8)) & 1;
}

} // namespace pl::sstv2::block
