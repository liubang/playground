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

#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pl::sstv2::types {

// =============================================================================
// Row: the user-facing input structure for SSTableBuilder::add().
//
// Users construct a Row with their key column values, version, operation type,
// and the raw value payload. SSTableBuilder is responsible for converting this
// into an InternalRow (encoding all-key, deciding embedded/separated,
// computing checksum, etc.).
//
// Invariants (enforced by SSTableBuilder, not by Row itself):
//   - key_columns.size() == schema.row_key_column_count()
//   - key_columns[i].type() matches schema.column_type(i)
//   - Rows must be added in strictly increasing all-key order.
// =============================================================================

struct Row {
    std::vector<Value> key_columns; // RowKey[0..M-1], must match Schema.
    uint64_t version = 0;           // Version (sorted descending internally).
    OpType op_type   = OpType::kPut;

    // Value payload metadata.
    DataType value_type = DataType::kNone;

    // Raw value bytes. Encoding depends on value_type:
    //   - Fixed-size scalars: little-endian bytes.
    //   - Strings/Binary: raw bytes (no length prefix).
    //   - Compound types: serialized per §13.2 of the spec.
    // Empty string with value_type=kNone means no value (e.g., for deletes).
    std::string value;
};

} // namespace pl::sstv2::types
