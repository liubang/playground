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
// Created: 2026/06/04 12:01

#include "cpp/pl/sstv2/types/schema.h"

#include <cassert>

namespace pl::sstv2::types {

// --- ExternalSchema ---

ExternalSchema::ExternalSchema(std::vector<ColumnDef> columns) : columns_(std::move(columns)) {
    assert(!columns_.empty());
}

size_t ExternalSchema::num_columns() const {
    return columns_.size();
}

const ColumnDef& ExternalSchema::column(size_t idx) const {
    return columns_[idx];
}

std::optional<size_t> ExternalSchema::find_column(std::string_view name) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

const ColumnDef& ExternalSchema::row_key_column() const {
    return columns_[0];
}

// --- InternalSchema ---

InternalSchema InternalSchema::from_external(const ExternalSchema& ext) {
    InternalSchema schema;
    schema.ranges_.reserve(ext.num_columns() + 1);

    for (size_t i = 0; i < ext.num_columns(); ++i) {
        schema.ranges_.push_back(schema.sub_columns_.size());
        const auto& col = ext.column(i);

        auto make_flag = [&](DataType dt, bool compound) -> Flag {
            return Flag{.type = dt, .compound_bit = compound, .bitmap_bit = col.nullable};
        };

        if (is_fixed_size(col.type)) {
            // Fixed-size types: 1 sub-column
            schema.sub_columns_.push_back({.name = col.name, .flag = make_flag(col.type, false)});
        } else if (is_variable_size(col.type)) {
            // String/Binary: 2 sub-columns (length + data)
            schema.sub_columns_.push_back(
                {.name = col.name + ".len", .flag = make_flag(DataType::kUint32, false)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".data", .flag = make_flag(col.type, false)});
        } else if (col.type == DataType::kArray) {
            // Array: 3 sub-columns (count + offsets + elements)
            DataType elem_type = col.element_type.value_or(DataType::kNone);
            schema.sub_columns_.push_back(
                {.name = col.name + ".count", .flag = make_flag(DataType::kUint32, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".offsets", .flag = make_flag(DataType::kUint32, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".elems", .flag = make_flag(elem_type, true)});
        } else if (col.type == DataType::kMap) {
            // Map: 5 sub-columns (count + key_offsets + keys + value_offsets +
            // values)
            DataType kt = col.key_type.value_or(DataType::kNone);
            DataType vt = col.value_type.value_or(DataType::kNone);
            schema.sub_columns_.push_back(
                {.name = col.name + ".count", .flag = make_flag(DataType::kUint32, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".key_offsets", .flag = make_flag(DataType::kUint32, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".keys", .flag = make_flag(kt, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".val_offsets", .flag = make_flag(DataType::kUint32, true)});
            schema.sub_columns_.push_back(
                {.name = col.name + ".vals", .flag = make_flag(vt, true)});
        } else {
            // Fallback: treat as opaque single sub-column
            schema.sub_columns_.push_back({.name = col.name, .flag = make_flag(col.type, false)});
        }
    }

    // Sentinel for computing the last column's range.
    schema.ranges_.push_back(schema.sub_columns_.size());
    return schema;
}

size_t InternalSchema::num_sub_columns() const {
    return sub_columns_.size();
}

Flag InternalSchema::flag(size_t sub_col_idx) const {
    return sub_columns_[sub_col_idx].flag;
}

std::string_view InternalSchema::sub_column_name(size_t sub_col_idx) const {
    return sub_columns_[sub_col_idx].name;
}

std::pair<size_t, size_t> InternalSchema::sub_column_range(size_t ext_col_idx) const {
    return {ranges_[ext_col_idx], ranges_[ext_col_idx + 1]};
}

} // namespace pl::sstv2::types
