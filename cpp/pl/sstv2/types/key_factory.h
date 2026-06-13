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
// Created: 2026/06/13 18:34

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/key_comparator.h"
#include "cpp/pl/sstv2/types/key_prefix.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// Key factory functions.
//
// These construct LogicalKey instances from higher-level inputs (Value vectors,
// InternalRows, KeyPrefixes), performing type validation against the schema.
// They depend on KeyComparator and are separated from key.h to avoid circular
// dependencies between key.h and key_comparator.h.
// =============================================================================

[[nodiscard]] inline absl::StatusOr<RowKey> make_row_key(std::vector<Value> columns,
                                                         const Schema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (columns.size() != schema->row_key_column_count()) {
        return absl::InvalidArgumentError("row key column count mismatch");
    }
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].type() != schema->column_type(i)) {
            return absl::InvalidArgumentError(absl::StrCat("row key column ", i, " type mismatch"));
        }
    }
    return RowKey::from_columns(std::move(columns));
}

[[nodiscard]] inline absl::StatusOr<AllKey> make_all_key(const InternalRow& row,
                                                         const InternalSchema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (row.columns.size() < schema->sort_key_column_count()) {
        return absl::InvalidArgumentError("internal row has fewer columns than all-key");
    }
    std::vector<Value> columns;
    columns.reserve(schema->sort_key_column_count());
    KeyComparator comparator(schema);
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        auto status = comparator.validate_sort_key_column(i, row.columns[i]);
        if (!status.ok())
            return status;
        columns.push_back(row.columns[i]);
    }
    return AllKey::from_columns(std::move(columns));
}

[[nodiscard]] inline absl::StatusOr<AllKeyView> make_all_key_view(
    const InternalRow& row, const InternalSchema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (row.columns.size() < schema->sort_key_column_count()) {
        return absl::InvalidArgumentError("internal row has fewer columns than all-key");
    }
    KeyComparator comparator(schema);
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        auto status = comparator.validate_sort_key_column(i, row.columns[i]);
        if (!status.ok())
            return status;
    }
    return AllKeyView::from_columns(row.columns.data(), schema->sort_key_column_count());
}

[[nodiscard]] inline absl::StatusOr<PrefixKey> make_prefix_key(
    const KeyPrefix& prefix,
    const Schema::ConstRef& schema,
    const InternalSchema::ConstRef& internal_schema) {
    if (internal_schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    auto shape_status = validate_key_prefix_shape(prefix, schema);
    if (!shape_status.ok())
        return shape_status;

    std::vector<Value> columns;
    columns.reserve(prefix.key_columns.size() + (prefix.version.has_value() ? 1 : 0) +
                    (prefix.op_type.has_value() ? 1 : 0));
    KeyComparator comparator(internal_schema);
    for (size_t i = 0; i < prefix.key_columns.size(); ++i) {
        auto status = comparator.validate_sort_key_column(i, prefix.key_columns[i]);
        if (!status.ok())
            return status;
        columns.push_back(prefix.key_columns[i]);
    }
    if (prefix.version.has_value()) {
        columns.push_back(Value::make<DataType::kVersion>(*prefix.version));
    }
    if (prefix.op_type.has_value()) {
        columns.push_back(Value::make<DataType::kUint8>(static_cast<uint8_t>(*prefix.op_type)));
    }
    for (size_t i = prefix.key_columns.size(); i < columns.size(); ++i) {
        auto status = comparator.validate_sort_key_column(i, columns[i]);
        if (!status.ok())
            return status;
    }
    return PrefixKey::from_columns(std::move(columns));
}

} // namespace pl::sstv2::types
