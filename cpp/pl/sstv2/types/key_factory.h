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
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/key_prefix.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// Key factory functions.
//
// Each factory has two overloads:
//   - const& : copies column Values from the source (safe when source is reused)
//   - &&     : moves column Values from the source (zero-copy when source is a
//              temporary, e.g. make_all_key(std::move(row), schema))
//
// Type validation is inlined rather than delegated to KeyComparator's
// per-column validate_sort_key_column, avoiding repeated null/bounds checks
// in the hot loop.
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

// ── make_all_key ──────────────────────────────────────────────────────────

namespace detail {

template <typename Row>
absl::StatusOr<AllKey> make_all_key_impl(Row&& row, const InternalSchema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (row.columns.size() < schema->sort_key_column_count()) {
        return absl::InvalidArgumentError("internal row has fewer columns than all-key");
    }
    std::vector<Value> columns;
    columns.reserve(schema->sort_key_column_count());
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        const DataType expected = schema->column_type(i);
        if (row.columns[i].type() != expected) {
            return absl::InvalidArgumentError(absl::StrCat("column ",
                                                           i,
                                                           " type mismatch: expected ",
                                                           data_type_name(expected),
                                                           ", got ",
                                                           data_type_name(row.columns[i].type())));
        }
        columns.push_back(std::forward<decltype(row.columns[i])>(row.columns[i]));
    }
    return AllKey::from_columns(std::move(columns));
}

} // namespace detail

[[nodiscard]] inline absl::StatusOr<AllKey> make_all_key(const InternalRow& row,
                                                         const InternalSchema::ConstRef& schema) {
    return detail::make_all_key_impl(row, schema); // copies
}

[[nodiscard]] inline absl::StatusOr<AllKey> make_all_key(InternalRow&& row,
                                                         const InternalSchema::ConstRef& schema) {
    return detail::make_all_key_impl(std::move(row), schema); // moves
}

// ── make_all_key_view ─────────────────────────────────────────────────────

[[nodiscard]] inline absl::StatusOr<AllKeyView> make_all_key_view(
    const InternalRow& row, const InternalSchema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (row.columns.size() < schema->sort_key_column_count()) {
        return absl::InvalidArgumentError("internal row has fewer columns than all-key");
    }
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        if (row.columns[i].type() != schema->column_type(i)) {
            return absl::InvalidArgumentError(absl::StrCat("column ",
                                                           i,
                                                           " type mismatch: expected ",
                                                           data_type_name(schema->column_type(i)),
                                                           ", got ",
                                                           data_type_name(row.columns[i].type())));
        }
    }
    return AllKeyView::from_columns(row.columns.data(), schema->sort_key_column_count());
}

// ── make_prefix_key ───────────────────────────────────────────────────────

namespace detail {

template <typename Prefix>
absl::StatusOr<PrefixKey> make_prefix_key_impl(Prefix&& prefix,
                                               const Schema::ConstRef& schema,
                                               const InternalSchema::ConstRef& internal_schema) {
    if (internal_schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    auto shape_status = validate_key_prefix_shape(prefix, schema);
    if (!shape_status.ok()) {
        return shape_status;
    }

    std::vector<Value> columns;
    columns.reserve(prefix.key_columns.size() + (prefix.version.has_value() ? 1 : 0) +
                    (prefix.op_type.has_value() ? 1 : 0));
    for (size_t i = 0; i < prefix.key_columns.size(); ++i) {
        const DataType expected = internal_schema->column_type(i);
        if (prefix.key_columns[i].type() != expected) {
            return absl::InvalidArgumentError(absl::StrCat("prefix column ",
                                                           i,
                                                           " type mismatch: expected ",
                                                           data_type_name(expected),
                                                           ", got ",
                                                           data_type_name(prefix.key_columns[i].type())));
        }
        columns.push_back(std::forward<decltype(prefix.key_columns[i])>(prefix.key_columns[i]));
    }
    if (prefix.version.has_value()) {
        columns.push_back(Value::make<DataType::kVersion>(*prefix.version));
    }
    if (prefix.op_type.has_value()) {
        columns.push_back(Value::make<DataType::kUint8>(static_cast<uint8_t>(*prefix.op_type)));
    }
    for (size_t i = prefix.key_columns.size(); i < columns.size(); ++i) {
        const DataType expected = internal_schema->column_type(i);
        if (columns[i].type() != expected) {
            return absl::InvalidArgumentError(absl::StrCat("prefix column ",
                                                           i,
                                                           " type mismatch: expected ",
                                                           data_type_name(expected),
                                                           ", got ",
                                                           data_type_name(columns[i].type())));
        }
    }
    return PrefixKey::from_columns(std::move(columns));
}

} // namespace detail

[[nodiscard]] inline absl::StatusOr<PrefixKey> make_prefix_key(
    const KeyPrefix& prefix,
    const Schema::ConstRef& schema,
    const InternalSchema::ConstRef& internal_schema) {
    return detail::make_prefix_key_impl(prefix, schema, internal_schema); // copies
}

[[nodiscard]] inline absl::StatusOr<PrefixKey> make_prefix_key(
    KeyPrefix&& prefix,
    const Schema::ConstRef& schema,
    const InternalSchema::ConstRef& internal_schema) {
    return detail::make_prefix_key_impl(std::move(prefix), schema, internal_schema); // moves
}

} // namespace pl::sstv2::types
