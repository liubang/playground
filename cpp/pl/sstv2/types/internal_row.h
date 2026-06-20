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

#include <cassert>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/types/column_flag.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::types {

// =============================================================================
// ValueLocation: where the value payload is physically stored.
//
// On wire, these map to the Filename column:
//   kEmbedded    -> "@1"
//   kKeyFile     -> "@2"  (index entries pointing within key file)
//   kValueFile   -> external path string
// =============================================================================

enum class ValueLocation : uint8_t {
    kEmbedded = 0,  // Value is embedded in the current block's data table.
    kKeyFile = 1,   // Value is elsewhere in the key file (index entries).
    kValueFile = 2, // Value is in an external value file.
};

constexpr std::string_view kEmbeddedFilename = "@1";
constexpr std::string_view kKeyFileFilename = "@2";

// =============================================================================
// InternalRow: the key-side M+7 column container of the Internal Table.
//
// This is a pure structural representation of one row in the Internal Table
// (design.md §7). It contains ONLY the M+7 column values:
//
//   columns[0..M-1]  = RowKey values
//   columns[M]       = Version (major, minor)
//   columns[M+1]     = OpType (uint8)
//   columns[M+2]     = Flag (uint64, raw bits of ColumnFlag)
//   columns[M+3]     = Filename (string: "@1", "@2", or external path)
//   columns[M+4]     = Offset (uint64)
//   columns[M+5]     = Length (uint64)
//   columns[M+6]     = Checksum (uint64)
//
// InternalRow does NOT carry:
//   - Value payload bytes (managed by BlockBuilder / ValueFileWriter)
//   - Precomputed all_key (computed on demand by the encoding layer)
//
// This separation enforces clean KV decoupling: InternalRow is purely the
// "key side" metadata. Value bytes flow through a separate channel.
// =============================================================================

struct InternalRow {
    using Ref = std::shared_ptr<InternalRow>;
    using ConstRef = std::shared_ptr<const InternalRow>;

    // All M+7 column values, indexed by InternalSchema column indices.
    std::vector<Value> columns;

    // =========================================================================
    // Typed accessors (convenience, require InternalSchema for index lookup).
    // =========================================================================

    [[nodiscard]] const Version& version(const InternalSchema::ConstRef& s) const {
        return columns[s->version_index()].ref<DataType::kVersion>();
    }

    [[nodiscard]] uint8_t op_type(const InternalSchema::ConstRef& s) const {
        return columns[s->op_type_index()].get<DataType::kUint8>();
    }

    [[nodiscard]] ColumnFlag flag(const InternalSchema::ConstRef& s) const {
        return ColumnFlag::from_raw(columns[s->flag_index()].get<DataType::kUint64>());
    }

    [[nodiscard]] std::string_view filename(const InternalSchema::ConstRef& s) const {
        return columns[s->filename_index()].ref<DataType::kString>();
    }

    [[nodiscard]] uint64_t offset(const InternalSchema::ConstRef& s) const {
        return columns[s->offset_index()].get<DataType::kUint64>();
    }

    [[nodiscard]] uint64_t length(const InternalSchema::ConstRef& s) const {
        return columns[s->length_index()].get<DataType::kUint64>();
    }

    [[nodiscard]] uint64_t checksum(const InternalSchema::ConstRef& s) const {
        return columns[s->checksum_index()].get<DataType::kUint64>();
    }

    // =========================================================================
    // Semantic queries.
    // =========================================================================

    [[nodiscard]] ValueLocation location(const InternalSchema::ConstRef& s) const {
        auto fn = filename(s);
        if (fn == kEmbeddedFilename) {
            return ValueLocation::kEmbedded;
        }
        if (fn == kKeyFileFilename) {
            return ValueLocation::kKeyFile;
        }
        return ValueLocation::kValueFile;
    }

    [[nodiscard]] bool is_embedded(const InternalSchema::ConstRef& s) const {
        return location(s) == ValueLocation::kEmbedded;
    }

    [[nodiscard]] bool is_index_entry(const InternalSchema::ConstRef& s) const {
        return flag(s).is_index_entry();
    }

    // =========================================================================
    // Construction helper.
    // =========================================================================

    static InternalRow make(const InternalSchema::ConstRef& s) {
        InternalRow row;
        row.columns.resize(s->column_count());
        return row;
    }
};

} // namespace pl::sstv2::types
