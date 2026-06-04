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

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/flag.h"

namespace pl::sstv2::types {

// Sort direction for key columns.
enum class SortOrder : uint8_t {
    kAscending = 0,
    kDescending = 1,
};

// User-provided column definition.
struct ColumnDef {
    std::string name;
    DataType type;
    bool nullable = false;
    std::optional<DataType> element_type; // Array element type
    std::optional<DataType> key_type;     // Map key type
    std::optional<DataType> value_type;   // Map value type
};

// Identifies a key column within ExternalSchema::columns_.
struct KeyColumnDef {
    size_t column_index;                         // index into columns_
    SortOrder order = SortOrder::kAscending;
};

// External schema: the user-facing view of table columns.
class ExternalSchema {
public:
    // columns: all columns (key + value columns).
    // key_columns: ordered list of columns forming the primary key.
    ExternalSchema(std::vector<ColumnDef> columns, std::vector<KeyColumnDef> key_columns);

    [[nodiscard]] size_t num_columns() const;
    [[nodiscard]] const ColumnDef& column(size_t idx) const;
    [[nodiscard]] std::optional<size_t> find_column(std::string_view name) const;

    // Primary key definition.
    [[nodiscard]] size_t num_key_columns() const;
    [[nodiscard]] const KeyColumnDef& key_column(size_t key_idx) const;
    [[nodiscard]] const std::vector<KeyColumnDef>& key_columns() const;

    // Convenience: type of the i-th key column.
    [[nodiscard]] DataType key_column_type(size_t key_idx) const;
    // Convenience: nullable of the i-th key column.
    [[nodiscard]] bool key_column_nullable(size_t key_idx) const;

    // Value columns = all columns not in key_columns.
    [[nodiscard]] std::vector<size_t> value_column_indices() const;

private:
    std::vector<ColumnDef> columns_;
    std::vector<KeyColumnDef> key_columns_;
};

// Internal schema: flattened representation of all columns decomposed
// into sub-columns suitable for columnar storage and pattern encoding.
class InternalSchema {
public:
    static InternalSchema from_external(const ExternalSchema& ext);

    [[nodiscard]] size_t num_sub_columns() const;
    [[nodiscard]] Flag flag(size_t sub_col_idx) const;
    [[nodiscard]] std::string_view sub_column_name(size_t sub_col_idx) const;

    // Maps external column index → sub-column range [start, end).
    [[nodiscard]] std::pair<size_t, size_t> sub_column_range(size_t ext_col_idx) const;

private:
    InternalSchema() = default;

    struct SubColumn {
        std::string name;
        Flag flag;
    };

    std::vector<SubColumn> sub_columns_;
    // For each external column, stores the starting sub-column index.
    // The range for column i is [ranges_[i], ranges_[i+1]).
    std::vector<size_t> ranges_;
};

} // namespace pl::sstv2::types
