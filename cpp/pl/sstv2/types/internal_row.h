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

#include <cassert>
#include <cstddef>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/variant.h"

namespace pl::sstv2::types {

// Row representation aligned to the internal (sub-column) schema.
// Wraps a vector of Variant values plus per-sub-column null flags.
class InternalRow {
public:
    explicit InternalRow(const InternalSchema& schema)
        : schema_(schema),
          values_(schema.num_sub_columns()),
          null_flags_(schema.num_sub_columns(), false) {}

    void set(size_t sub_col_idx, Variant value) {
        assert(sub_col_idx < values_.size());
        values_[sub_col_idx] = std::move(value);
        null_flags_[sub_col_idx] = false;
    }

    void set_null(size_t sub_col_idx) {
        assert(sub_col_idx < values_.size());
        values_[sub_col_idx] = Variant::none();
        null_flags_[sub_col_idx] = true;
    }

    [[nodiscard]] const Variant& get(size_t sub_col_idx) const {
        assert(sub_col_idx < values_.size());
        return values_[sub_col_idx];
    }

    [[nodiscard]] bool is_null(size_t sub_col_idx) const {
        assert(sub_col_idx < null_flags_.size());
        return null_flags_[sub_col_idx];
    }

    // row_key is always the first sub-column of the first external column.
    [[nodiscard]] std::string_view row_key() const { return values_[0].as_string(); }

private:
    const InternalSchema& schema_;
    std::vector<Variant> values_;
    std::vector<bool> null_flags_;
};

} // namespace pl::sstv2::types
