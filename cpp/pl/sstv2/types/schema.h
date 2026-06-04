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

// User-provided column definition.
struct ColumnDef {
    std::string name;
    DataType type;
    bool nullable = false;
    std::optional<DataType> element_type; // Array element type
    std::optional<DataType> key_type;     // Map key type
    std::optional<DataType> value_type;   // Map value type
};

// External schema: the user-facing view of table columns.
class ExternalSchema {
public:
    explicit ExternalSchema(std::vector<ColumnDef> columns);

    [[nodiscard]] size_t num_columns() const;
    [[nodiscard]] const ColumnDef& column(size_t idx) const;
    [[nodiscard]] std::optional<size_t> find_column(std::string_view name) const;

    // row_key is always the first column.
    [[nodiscard]] const ColumnDef& row_key_column() const;

private:
    std::vector<ColumnDef> columns_;
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
