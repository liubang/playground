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
// Created: 2026/06/12 00:00

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

struct RowKeyTag {};
struct AllKeyTag {};
struct PrefixKeyTag {};

template <typename Tag> class LogicalKey {
public:
    using tag = Tag;
    using Ref = std::shared_ptr<LogicalKey>;
    using ConstRef = std::shared_ptr<const LogicalKey>;

    LogicalKey() = default;

    [[nodiscard]] static LogicalKey from_columns(std::vector<Value> columns) {
        return LogicalKey(std::move(columns));
    }

    [[nodiscard]] size_t column_count() const noexcept { return columns_.size(); }
    [[nodiscard]] bool empty() const noexcept { return columns_.empty(); }
    [[nodiscard]] const Value& column(size_t index) const { return columns_[index]; }
    [[nodiscard]] const std::vector<Value>& columns() const noexcept { return columns_; }

private:
    explicit LogicalKey(std::vector<Value> columns) : columns_(std::move(columns)) {}

    std::vector<Value> columns_;
};

template <typename Tag> class KeyView {
public:
    using tag = Tag;

    KeyView() = default;

    [[nodiscard]] static KeyView from_columns(const Value* columns, size_t column_count) {
        return KeyView(columns, column_count);
    }

    [[nodiscard]] size_t column_count() const noexcept { return column_count_; }
    [[nodiscard]] bool empty() const noexcept { return column_count_ == 0; }
    [[nodiscard]] const Value& column(size_t index) const { return columns_[index]; }

private:
    KeyView(const Value* columns, size_t column_count)
        : columns_(columns), column_count_(column_count) {}

    const Value* columns_ = nullptr;
    size_t column_count_ = 0;
};

template <typename Tag> class EncodedKey {
public:
    using tag = Tag;
    using Ref = std::shared_ptr<EncodedKey>;
    using ConstRef = std::shared_ptr<const EncodedKey>;

    EncodedKey() = default;

    [[nodiscard]] static EncodedKey from_encoded_bytes(std::string bytes) {
        return EncodedKey(std::move(bytes));
    }

    [[nodiscard]] std::string_view bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::string& owned_bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

private:
    explicit EncodedKey(std::string bytes) : bytes_(std::move(bytes)) {}

    std::string bytes_;
};

using RowKey = LogicalKey<RowKeyTag>;
using AllKey = LogicalKey<AllKeyTag>;
using PrefixKey = LogicalKey<PrefixKeyTag>;

using RowKeyView = KeyView<RowKeyTag>;
using AllKeyView = KeyView<AllKeyTag>;
using PrefixKeyView = KeyView<PrefixKeyTag>;

using EncodedRowKey = EncodedKey<RowKeyTag>;
using EncodedAllKey = EncodedKey<AllKeyTag>;
using EncodedPrefixKey = EncodedKey<PrefixKeyTag>;

template <typename LhsTag, typename RhsTag>
[[nodiscard]] inline int compare_encoded_bytes(const EncodedKey<LhsTag>& lhs,
                                               const EncodedKey<RhsTag>& rhs) noexcept {
    const int cmp = lhs.bytes().compare(rhs.bytes());
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    return 0;
}

template <typename LhsTag, typename RhsTag>
[[nodiscard]] inline bool encoded_bytes_less(const EncodedKey<LhsTag>& lhs,
                                             const EncodedKey<RhsTag>& rhs) noexcept {
    return compare_encoded_bytes(lhs, rhs) < 0;
}

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

template <typename Key>
concept KeyLike = requires(const Key& key, size_t index) {
    typename Key::tag;
    { key.column_count() } -> std::convertible_to<size_t>;
    { key.column(index) } -> std::same_as<const Value&>;
};

template <typename Key, typename Tag>
concept KeyWithTag = KeyLike<Key> && std::same_as<typename Key::tag, Tag>;

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
        if (!cmp.ok())
            return cmp.status();
        return *cmp < 0;
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, AllKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<bool> all_key_less_than_prefix(const Lhs& lhs,
                                                                const Rhs& rhs) const {
        auto cmp = compare_all_key_to_prefix(lhs, rhs);
        if (!cmp.ok())
            return cmp.status();
        return *cmp < 0;
    }

    template <typename Lhs, typename Rhs>
        requires KeyWithTag<Lhs, PrefixKeyTag> && KeyWithTag<Rhs, PrefixKeyTag>
    [[nodiscard]] absl::StatusOr<bool> prefix_less(const Lhs& lhs, const Rhs& rhs) const {
        auto cmp = compare_prefix_boundary(lhs, rhs);
        if (!cmp.ok())
            return cmp.status();
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
            auto status = validate_sort_key_column(i, lhs.column(i));
            if (!status.ok())
                return status;
            status = validate_sort_key_column(i, rhs.column(i));
            if (!status.ok())
                return status;
            int cmp = compare_values(lhs.column(i), rhs.column(i));
            if (schema_->column_order(i) == SortOrder::kDescending) {
                cmp = -cmp;
            }
            if (cmp != 0)
                return cmp;
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
