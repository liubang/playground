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
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// Key type tags — distinguish between the three key scopes at compile time.
struct RowKeyTag {};
struct AllKeyTag {};
struct PrefixKeyTag {};

// =============================================================================
// SystemKey: the system-managed portion of the all-key.
// =============================================================================

struct SystemKey {
    static constexpr size_t kColumnCount = 2; // Version + OpType
    Version version;
    OpType op_type = OpType::kPut;
    bool operator==(const SystemKey&) const = default;
};

// =============================================================================
// KeyView<Tag>: a zero-copy view into a LogicalKey's columns.
//
// Defined before LogicalKey so that LogicalKey's AllKey-specific accessors
// can return KeyView<RowKeyTag>.
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
// LogicalKey<Tag>: a key composed of typed Value columns.
//
// Each column stores a full Value. The Tag parameter enables compile-time
// dispatch: RowKeyTag / AllKeyTag / PrefixKeyTag.
//
// AllKeyTag adds spec-level accessors (row_key_view, system_key) via
// requires-clauses — zero overhead for RowKey / PrefixKey instantiations.
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

    // Move the column vector out — enables zero-copy composition in Row::create().
    [[nodiscard]] std::vector<Value> release_columns() && { return std::move(columns_); }

    // ── AllKey-specific accessors ──────────────────────────────────────
    // Only available on LogicalKey<AllKeyTag>.  The requires-clause
    // rejects RowKey / PrefixKey at compile time with a clear message.

    // Zero-copy view over the RowKey portion: columns [0, N-2).
    [[nodiscard]] KeyView<RowKeyTag> row_key_view() const
        requires std::same_as<Tag, AllKeyTag>
    {
        const size_t n = columns_.size();
        assert(n >= SystemKey::kColumnCount);
        return KeyView<RowKeyTag>::from_columns(columns_.data(), n - SystemKey::kColumnCount);
    }

    // Extract SystemKey from the last 2 columns.
    [[nodiscard]] SystemKey system_key() const
        requires std::same_as<Tag, AllKeyTag>
    {
        const size_t n = columns_.size();
        assert(n >= SystemKey::kColumnCount);
        return SystemKey{
            .version = columns_[n - 2].template ref<DataType::kVersion>(),
            .op_type = static_cast<OpType>(columns_[n - 1].template get<DataType::kUint8>()),
        };
    }

private:
    explicit LogicalKey(std::vector<Value> columns) : columns_(std::move(columns)) {}
    std::vector<Value> columns_;
};

// =============================================================================
// EncodedKey<Tag>: pre-computed memcomparable encoding of a LogicalKey.
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
// Type aliases — three key scopes × three representations.
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

} // namespace pl::sstv2::types
