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
// Created: 2026/06/13 18:35

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// KeyPrefix: a partial key used for range-scan boundaries.
//
// A KeyPrefix specifies a prefix of the full AllKey for range queries:
//   - key_columns: prefix of row key columns (0..M).
//   - version: optional version component (requires complete row key prefix).
//   - op_type: optional operation type component (requires version).
//
// Example:
//   KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
//                              Value::make<DataType::kUint64>(42)},
//             .version = Version{.major = 10}}
// =============================================================================

struct KeyPrefix {
    using Ref = std::shared_ptr<KeyPrefix>;
    using ConstRef = std::shared_ptr<const KeyPrefix>;

    std::vector<Value> key_columns;
    std::optional<Version> version;
    std::optional<OpType> op_type;
};

[[nodiscard]] inline absl::Status validate_key_prefix_shape(const KeyPrefix& prefix,
                                                            const Schema::ConstRef& schema) {
    if (schema == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    const size_t row_key_count = schema->row_key_column_count();
    if (prefix.key_columns.size() > row_key_count) {
        return absl::InvalidArgumentError("key prefix column count exceeds row key column count");
    }
    if (prefix.version.has_value() && prefix.key_columns.size() != row_key_count) {
        return absl::InvalidArgumentError("version prefix requires a complete row key prefix");
    }
    if (prefix.op_type.has_value() && !prefix.version.has_value()) {
        return absl::InvalidArgumentError("op type prefix requires a version prefix");
    }
    return absl::OkStatus();
}

} // namespace pl::sstv2::types
