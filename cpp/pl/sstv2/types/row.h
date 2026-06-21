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

#include <memory>

#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// Row = AllKey + Value
//
// Row maps directly to the SSTableV2 spec: one user-table row consists of a
// complete all-key (RowKey + SystemKey) and a single value column.
//
// Multi-column values are supported by encoding them into a single Value via
// Array/Map, or by using the ColumnName-in-RowKey normalization technique
// described in the SSTableV2 spec §2.1.
//
// SSTableBuilder is responsible for converting this into an InternalRow
// (serializing the value, computing checksum, deciding embedded/separated).
//
// Invariants (enforced by SSTableBuilder, not by Row itself):
//   - all_key.row_key_view().column_count() == schema.row_key_column_count()
//   - all_key.row_key_view().column(i).type() matches schema.column_type(i)
//   - Rows must be added in strictly increasing all-key order.
//   - For delete tombstones, value should be default (null).
// =============================================================================

struct Row {
    using Ref = std::shared_ptr<Row>;
    using ConstRef = std::shared_ptr<const Row>;

    AllKey all_key;
    Value value;

    // -------------------------------------------------------------------------
    // Factory: create a Row from its spec-level components.
    // -------------------------------------------------------------------------
    // This is the primary user-facing construction API: caller provides a RowKey
    // (user key columns), SystemKey (version + op_type), and a Value.
    // The factory merges them into a single AllKey internally.

    // Takes RowKey by rvalue-ref — moves the column vector out directly,
    // avoiding a deep copy. The typical call site is:
    //   Row::create(RowKey::from_columns({...}), SystemKey{...}, Value::make<...>(...))
    // where RowKey::from_columns returns a temporary.
    [[nodiscard]] static Row create(RowKey&& row_key, SystemKey system_key, Value value = {}) {
        std::vector<Value> all_key_cols = std::move(row_key).release_columns();
        all_key_cols.reserve(all_key_cols.size() + SystemKey::kColumnCount);
        all_key_cols.push_back(Value::make<DataType::kVersion>(system_key.version));
        all_key_cols.push_back(
            Value::make<DataType::kUint8>(static_cast<uint8_t>(system_key.op_type)));
        return Row{.all_key = AllKey::from_columns(std::move(all_key_cols)),
                   .value = std::move(value)};
    }

    // -------------------------------------------------------------------------
    // Factory: create a Row from a pre-built AllKey (internal / deserialization).
    // -------------------------------------------------------------------------
    [[nodiscard]] static Row from_all_key(AllKey all_key, Value value = {}) {
        return Row{.all_key = std::move(all_key), .value = std::move(value)};
    }

    // -------------------------------------------------------------------------
    // Convenience accessors — delegate to AllKey components.
    // -------------------------------------------------------------------------

    [[nodiscard]] RowKeyView row_key() const { return all_key.row_key_view(); }
    [[nodiscard]] SystemKey system_key() const { return all_key.system_key(); }
};

} // namespace pl::sstv2::types
