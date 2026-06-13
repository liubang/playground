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
#include "cpp/pl/sstv2/types/key_prefix.h"
#include "cpp/pl/sstv2/types/key_tags.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// LogicalKey<Tag>: a key composed of typed Value columns.
//
// Each column stores the full Value (type + payload). This is the primary
// key representation used during comparison and for AllKey construction.
// =============================================================================

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

// =============================================================================
// KeyView<Tag>: a zero-copy view into a LogicalKey's columns.
//
// Holds a pointer into the parent key — caller must keep the parent alive.
// =============================================================================

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

// =============================================================================
// EncodedKey<Tag>: a pre-computed memcomparable encoding of a LogicalKey.
//
// Stores the comparable-encoded bytes (see codec/value_comparable.h) for
// efficient comparison and use as bloom filter keys.
// =============================================================================

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

// =============================================================================
// Type aliases — the three key scopes, each with three representations.
// =============================================================================

using RowKey = LogicalKey<RowKeyTag>;
using AllKey = LogicalKey<AllKeyTag>;
using PrefixKey = LogicalKey<PrefixKeyTag>;

using RowKeyView = KeyView<RowKeyTag>;
using AllKeyView = KeyView<AllKeyTag>;
using PrefixKeyView = KeyView<PrefixKeyTag>;

using EncodedRowKey = EncodedKey<RowKeyTag>;
using EncodedAllKey = EncodedKey<AllKeyTag>;
using EncodedPrefixKey = EncodedKey<PrefixKeyTag>;

// =============================================================================
// Encoded key comparison helpers.
// =============================================================================

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

} // namespace pl::sstv2::types
