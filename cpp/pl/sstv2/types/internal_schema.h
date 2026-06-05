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
// Created: 2026/06/05 00:45

#pragma once

#include "cpp/pl/sstv2/types/schema.h"

#include <cassert>
#include <cstddef>

namespace pl::sstv2::types {

// =============================================================================
// InternalSchema: the complete M+7 column view of the internal table.
//
// Given a user Schema with M row key columns, InternalSchema provides a
// unified column interface over the full internal table:
//
//   Index       Name        Type       SortOrder
//   -----       ----        ----       ---------
//   0..M-1      (user)      (user)     (user)
//   M           Version     kUint64    kDescending
//   M+1         OpType      kUint8     kAscending
//   M+2         Flag        kUint64    (none, not sorted)
//   M+3         Filename    kString    (none)
//   M+4         Offset      kUint64    (none)
//   M+5         Length      kUint64    (none)
//   M+6         Checksum    kUint64    (none)
//
// The first M+2 columns (RowKey + Version + OpType) participate in the
// all_key sort order. The remaining 5 columns (Flag..Checksum) are payload
// columns that do not participate in sorting.
//
// InternalSchema does NOT own the user Schema — it holds a const reference.
// The user Schema must outlive the InternalSchema.
// =============================================================================

class InternalSchema {
public:
    explicit InternalSchema(const Schema& user_schema)
        : user_schema_(user_schema) {}

    // =========================================================================
    // Column count.
    // =========================================================================

    // Number of user-defined row key columns (M).
    [[nodiscard]] size_t user_column_count() const { return user_schema_.row_key_column_count(); }

    // Total columns in the internal table (M + 7).
    [[nodiscard]] size_t column_count() const { return user_schema_.row_key_column_count() + kSystemColumnCount; }

    // Number of columns that participate in the all_key (M + 2: RowKey + Version + OpType).
    [[nodiscard]] size_t sort_key_column_count() const { return user_schema_.row_key_column_count() + 2; }

    // =========================================================================
    // Unified column access.
    // =========================================================================

    // Get the ColumnDef for any column index in [0, M+7).
    // For user columns, delegates to the user schema.
    // For system columns, returns the predefined definitions.
    [[nodiscard]] const ColumnDef& column(size_t index) const {
        size_t m = user_schema_.row_key_column_count();
        if (index < m) {
            return user_schema_.column(index);
        }
        assert(index < m + kSystemColumnCount);
        return kSystemColumns[index - m];
    }

    [[nodiscard]] DataType column_type(size_t index) const { return column(index).type; }
    [[nodiscard]] SortOrder column_order(size_t index) const { return column(index).order; }
    [[nodiscard]] std::string_view column_name(size_t index) const { return column(index).name; }

    // =========================================================================
    // Named system column indices.
    // =========================================================================

    [[nodiscard]] size_t version_index() const  { return user_schema_.row_key_column_count(); }
    [[nodiscard]] size_t op_type_index() const  { return user_schema_.row_key_column_count() + 1; }
    [[nodiscard]] size_t flag_index() const     { return user_schema_.row_key_column_count() + 2; }
    [[nodiscard]] size_t filename_index() const { return user_schema_.row_key_column_count() + 3; }
    [[nodiscard]] size_t offset_index() const   { return user_schema_.row_key_column_count() + 4; }
    [[nodiscard]] size_t length_index() const   { return user_schema_.row_key_column_count() + 5; }
    [[nodiscard]] size_t checksum_index() const { return user_schema_.row_key_column_count() + 6; }

    // =========================================================================
    // Query: does this column participate in sorting?
    // =========================================================================

    [[nodiscard]] bool is_sort_column(size_t index) const { return index < sort_key_column_count(); }

    // =========================================================================
    // Access to the underlying user schema.
    // =========================================================================

    [[nodiscard]] const Schema& user_schema() const { return user_schema_; }

    // =========================================================================
    // Constants.
    // =========================================================================

    static constexpr size_t kSystemColumnCount = 7;

private:
    const Schema& user_schema_;

    // Predefined system column definitions.
    // These are static because they never change across schemas.
    // SortOrder for non-sort columns is kAscending by convention (unused).
    static inline const ColumnDef kSystemColumns[kSystemColumnCount] = {
        {.name="Version",  .type=DataType::kUint64, .order=SortOrder::kDescending},
        {.name="OpType",   .type=DataType::kUint8,  .order=SortOrder::kAscending},
        {.name="Flag",     .type=DataType::kUint64, .order=SortOrder::kAscending},
        {.name="Filename", .type=DataType::kString, .order=SortOrder::kAscending},
        {.name="Offset",   .type=DataType::kUint64, .order=SortOrder::kAscending},
        {.name="Length",   .type=DataType::kUint64, .order=SortOrder::kAscending},
        {.name="Checksum", .type=DataType::kUint64, .order=SortOrder::kAscending},
    };
};

} // namespace pl::sstv2::types
