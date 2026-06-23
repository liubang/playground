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

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// Key concepts: compile-time constraints for key comparison.
// =============================================================================

template <typename Key>
concept KeyLike = requires(const Key& key, size_t index) {
    typename Key::tag;
    { key.column_count() } -> std::convertible_to<size_t>;
    { key.column(index) } -> std::same_as<const Value&>;
};

template <typename Key, typename Tag>
concept KeyWithTag = KeyLike<Key> && std::same_as<typename Key::tag, Tag>;

// =============================================================================
// KeyComparator: lexicographic comparison over AllKey columns.
//
// Performs type-checked column-by-column comparison respecting SortOrder
// (ascending/descending). Supports exact boundary matching for AllKey vs
// AllKey and prefix matching for AllKey prefix queries.
//
// Usage:
//   KeyComparator cmp(internal_schema);
//   auto result = cmp.compare_all_key(lhs_all_key, rhs_all_key);
//   if (result.ok() && *result < 0) { ... lhs < rhs ... }
// =============================================================================

class KeyComparator {
public:
    explicit KeyComparator(InternalSchema::ConstRef schema) : schema_(std::move(schema)) {}

    [[nodiscard]] absl::Status validate_sort_key_column(size_t index, const Value& value) const {
        if (schema_ == nullptr) {
            return absl::InvalidArgumentError("schema is null");
        }
        if (index >= schema_->sort_key_column_count()) {
            return absl::InvalidArgumentError("key column index exceeds all-key column count");
        }
        const DataType expected = schema_->column_type(index);
        if (value.type() != expected) {
            return absl::InvalidArgumentError(absl::StrCat("key column ",
                                                           index,
                                                           " type mismatch: expected ",
                                                           data_type_name(expected),
                                                           ", got ",
                                                           data_type_name(value.type())));
        }
        return absl::OkStatus();
    }

    template <KeyLike Lhs, KeyLike Rhs>
    [[nodiscard]] absl::StatusOr<int> compare_exact(const Lhs& lhs, const Rhs& rhs) const {
        return compare_impl(lhs, rhs, PrefixMatchMode::kExactBoundary);
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, AllKeyTag>
    [[nodiscard]] absl::StatusOr<int> compare_all_key(const Lhs& lhs, const Rhs& rhs) const {
        return compare_impl(lhs, rhs, PrefixMatchMode::kExactBoundary);
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<int> compare_all_key_to_prefix(const Lhs& lhs,
                                                                const Rhs& rhs) const {
        return compare_impl(lhs, rhs, PrefixMatchMode::kPrefixMatches);
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, PrefixKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<int> compare_prefix_boundary(const Lhs& lhs,
                                                              const Rhs& rhs) const {
        return compare_impl(lhs, rhs, PrefixMatchMode::kExactBoundary);
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, AllKeyTag>
    [[nodiscard]] absl::StatusOr<bool> all_key_less(const Lhs& lhs, const Rhs& rhs) const {
        auto cmp = compare_all_key(lhs, rhs);
        if (!cmp.ok()) {
            return cmp.status();
        }
        return *cmp < 0;
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<bool> all_key_less_than_prefix(const Lhs& lhs,
                                                                const Rhs& rhs) const {
        auto cmp = compare_all_key_to_prefix(lhs, rhs);
        if (!cmp.ok()) {
            return cmp.status();
        }
        return *cmp < 0;
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, PrefixKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<bool> prefix_less(const Lhs& lhs, const Rhs& rhs) const {
        auto cmp = compare_prefix_boundary(lhs, rhs);
        if (!cmp.ok()) {
            return cmp.status();
        }
        return *cmp < 0;
    }

private:
    enum class PrefixMatchMode : uint8_t {
        kExactBoundary,
        kPrefixMatches,
    };

    template <KeyLike Lhs, KeyLike Rhs>
    [[nodiscard]] absl::StatusOr<int> compare_impl(const Lhs& lhs,
                                                   const Rhs& rhs,
                                                   PrefixMatchMode mode) const {
        if (schema_ == nullptr) {
            return absl::InvalidArgumentError("schema is null");
        }

        const size_t common = std::min(lhs.column_count(), rhs.column_count());
        for (size_t i = 0; i < common; ++i) {
            const DataType expected = schema_->column_type(i);
            if (lhs.column(i).type() != expected || rhs.column(i).type() != expected) {
                return absl::InvalidArgumentError(absl::StrCat(
                    "key column ", i, " type mismatch: expected ", data_type_name(expected)));
            }
            int cmp = compare_values(lhs.column(i), rhs.column(i));
            if (schema_->column_order(i) == SortOrder::kDescending) {
                cmp = -cmp;
            }
            if (cmp != 0) {
                return cmp;
            }
        }

        if (lhs.column_count() == rhs.column_count()) {
            return 0;
        }
        if (mode == PrefixMatchMode::kPrefixMatches) {
            return 0;
        }
        return lhs.column_count() < rhs.column_count() ? -1 : 1;
    }

    InternalSchema::ConstRef schema_;
};

} // namespace pl::sstv2::types
