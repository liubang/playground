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
// Created: 2026/06/05 00:23

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::types {

// =============================================================================
// SortOrder: the ordering direction for a row key column.
// =============================================================================

enum class SortOrder : uint8_t {
    kAscending = 0,
    kDescending = 1,
};

// =============================================================================
// ColumnDef: definition of a single column.
//
// Each column has a name (used in metadata serialization), a DataType,
// and a sort direction. For non-sort columns, order is conventionally
// kAscending but unused.
// =============================================================================

struct ColumnDef {
    std::string name;
    DataType type = DataType::kNone;
    SortOrder order = SortOrder::kAscending;
};

// =============================================================================
// Schema: immutable description of the user-defined row key structure.
//
// This is the "external schema" — it describes only the M row key columns
// that the user defines. The 7 system columns (Version, OpType, Flag,
// Filename, Offset, Length, Checksum) are NOT part of Schema; they are
// introduced by InternalSchema which wraps a Schema.
//
// Schema is constructed via SchemaBuilder to enforce invariants at build time.
// Once constructed, a Schema is immutable.
// =============================================================================

class Schema {
public:
    // Default: empty schema (zero row key columns).
    Schema() = default;

    // Direct construction (prefer SchemaBuilder for validation).
    explicit Schema(std::vector<ColumnDef> columns) : columns_(std::move(columns)) {}

    // =========================================================================
    // Row key column access.
    // =========================================================================

    [[nodiscard]] size_t row_key_column_count() const { return columns_.size(); }

    [[nodiscard]] const ColumnDef& column(size_t index) const { return columns_[index]; }

    [[nodiscard]] std::string_view column_name(size_t index) const { return columns_[index].name; }
    [[nodiscard]] DataType column_type(size_t index) const { return columns_[index].type; }
    [[nodiscard]] SortOrder column_order(size_t index) const { return columns_[index].order; }

    [[nodiscard]] const std::vector<ColumnDef>& columns() const { return columns_; }

    // =========================================================================
    // Iteration (range-for support).
    // =========================================================================

    [[nodiscard]] auto begin() const { return columns_.begin(); }
    [[nodiscard]] auto end() const { return columns_.end(); }

private:
    std::vector<ColumnDef> columns_;
};

// =============================================================================
// SchemaBuilder: validated, fluent construction of Schema objects.
//
// Usage:
//   auto schema = SchemaBuilder()
//       .add_column("user_id", DataType::kUint64)
//       .add_column("timestamp", DataType::kInt64, SortOrder::kDescending)
//       .build();
//
// build() returns a valid Schema. If validation fails (e.g., duplicate names,
// invalid types), build() returns an empty Schema — check error() for details.
// =============================================================================

class SchemaBuilder {
public:
    SchemaBuilder() = default;

    SchemaBuilder& add_column(std::string name,
                              DataType type,
                              SortOrder order = SortOrder::kAscending) {
        columns_.push_back(ColumnDef{.name = std::move(name), .type = type, .order = order});
        return *this;
    }

    // Validate and build. Returns nullopt on failure (check error()).
    std::optional<Schema> build() {
        error_.clear();
        if (!validate()) {
            return std::nullopt;
        }
        return Schema{std::move(columns_)};
    }

    // After a failed build(), returns the validation error message.
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    bool validate() {
        if (columns_.empty()) {
            error_ = "schema must contain at least one row key column";
            return false;
        }
        for (size_t i = 0; i < columns_.size(); ++i) {
            const auto& col = columns_[i];
            if (col.name.empty()) {
                error_ = "column " + std::to_string(i) + " has empty name";
                return false;
            }
            if (!is_key_compatible(col.type)) {
                error_ = "column '" + col.name + "' has non-key-compatible type " +
                         std::string(data_type_name(col.type));
                return false;
            }
            // Check for duplicate names.
            for (size_t j = 0; j < i; ++j) {
                if (columns_[j].name == col.name) {
                    error_ = "duplicate column name '" + col.name + "'";
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<ColumnDef> columns_;
    std::string error_;
};

} // namespace pl::sstv2::types
