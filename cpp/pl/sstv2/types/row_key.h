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

#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/variant.h"

namespace pl::sstv2::types {

// A structured row key consisting of 1..N typed column values.
// Holds a reference to ExternalSchema for self-aware encoding, comparison, and validation.
// The schema must outlive the RowKey.
class RowKey {
public:
    // --- Construction (with schema validation) ---

    // Construct a full key or prefix key with strict validation.
    // Validates:
    //   1. columns.size() > 0 && columns.size() <= schema.num_key_columns()
    //   2. Each column's type matches the corresponding key column type in schema
    //   3. Non-nullable key columns must not be null (Variant::is_none())
    // Returns error status if validation fails.
    static absl::StatusOr<RowKey> create(const ExternalSchema& schema,
                                         std::vector<Variant> columns);

    // Unchecked construction for internal use (e.g., decoding from known-good bytes).
    // Caller guarantees type safety. No validation performed.
    static RowKey create_unchecked(const ExternalSchema& schema, std::vector<Variant> columns);

    // --- Accessors ---

    [[nodiscard]] size_t num_columns() const;
    [[nodiscard]] const Variant& column(size_t idx) const;
    [[nodiscard]] const Variant& operator[](size_t idx) const;
    [[nodiscard]] bool empty() const;

    // Is this a prefix key (fewer columns than schema defines)?
    [[nodiscard]] bool is_prefix() const;

    // Is this a full key (all key columns present)?
    [[nodiscard]] bool is_full() const;

    [[nodiscard]] const ExternalSchema& schema() const;

    // --- Comparison (respects sort orders from schema) ---

    // Compare with another RowKey. Both must reference the same schema.
    // Prefix semantics: compare overlapping columns; if all equal, fewer columns < more columns.
    // SortOrder::kDescending inverts the comparison for that column.
    [[nodiscard]] std::strong_ordering compare(const RowKey& other) const;

    bool operator==(const RowKey& other) const;
    bool operator<(const RowKey& other) const;
    bool operator<=(const RowKey& other) const;
    bool operator>(const RowKey& other) const;
    bool operator>=(const RowKey& other) const;

    // Check if this key is a prefix of `other`.
    [[nodiscard]] bool is_prefix_of(const RowKey& other) const;

    // --- Encoding / Decoding (memcomparable format) ---

    // Encode to memcomparable bytes suitable for byte-wise comparison.
    void encode_to(std::string& out) const;
    [[nodiscard]] std::string encode() const;

    // Decode from memcomparable bytes back to structured RowKey.
    // Decodes all key columns present in the encoded data.
    static absl::StatusOr<RowKey> decode(const ExternalSchema& schema, std::string_view encoded);

private:
    RowKey(const ExternalSchema& schema, std::vector<Variant> columns);

    const ExternalSchema* schema_;
    std::vector<Variant> columns_;
};

} // namespace pl::sstv2::types
