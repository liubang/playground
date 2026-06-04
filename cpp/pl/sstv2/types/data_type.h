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

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace pl::sstv2::types {

// =============================================================================
// DataType: the fundamental type discriminator for SSTableV2 column values.
//
// Enum values 0-20 are public types usable in user schemas.
// Values 21-22 are private types used only in index block entries.
// =============================================================================

enum class DataType : uint8_t {
    kNone        = 0,
    kBool        = 1,
    kInt8        = 2,
    kUint8       = 3,
    kInt16       = 4,
    kUint16      = 5,
    kInt32       = 6,
    kUint32      = 7,
    kInt64       = 8,
    kUint64      = 9,
    kFloat       = 10,
    kDouble      = 11,
    kLongDouble  = 12,
    kTime        = 13,
    kVersion     = 14,
    kString      = 15,
    kU16String   = 16,
    kU32String   = 17,
    kBinary      = 18,
    kArray       = 19,
    kMap         = 20,
    // Private types for index block Flag field.
    kDataBlock   = 21,
    kIndexBlock  = 22,
};

// =============================================================================
// Compile-time type classification traits.
//
// These are constexpr predicates that enable both runtime branching and
// template-based static dispatch on DataType categories.
// =============================================================================

constexpr bool is_signed_integer(DataType dt) {
    return dt == DataType::kInt8 || dt == DataType::kInt16 ||
           dt == DataType::kInt32 || dt == DataType::kInt64;
}

constexpr bool is_unsigned_integer(DataType dt) {
    return dt == DataType::kUint8 || dt == DataType::kUint16 ||
           dt == DataType::kUint32 || dt == DataType::kUint64;
}

constexpr bool is_integer(DataType dt) {
    return is_signed_integer(dt) || is_unsigned_integer(dt);
}

constexpr bool is_floating_point(DataType dt) {
    return dt == DataType::kFloat || dt == DataType::kDouble ||
           dt == DataType::kLongDouble;
}

constexpr bool is_numeric(DataType dt) {
    return is_integer(dt) || is_floating_point(dt);
}

// Fixed-size types have a statically known wire size (no length prefix needed).
constexpr bool is_fixed_size(DataType dt) {
    return dt == DataType::kBool || is_integer(dt) || is_floating_point(dt);
}

// Variable-length types require a length prefix or (offset, length) pair.
constexpr bool is_variable_length(DataType dt) {
    return dt == DataType::kString || dt == DataType::kU16String ||
           dt == DataType::kU32String || dt == DataType::kBinary ||
           dt == DataType::kArray || dt == DataType::kMap;
}

// String-like types (all variable-length text encodings + binary).
constexpr bool is_string_like(DataType dt) {
    return dt == DataType::kString || dt == DataType::kU16String ||
           dt == DataType::kU32String || dt == DataType::kBinary;
}

// Compound types that decompose into sub-columns in column-store encoding.
constexpr bool is_compound(DataType dt) {
    return dt == DataType::kTime || dt == DataType::kVersion ||
           dt == DataType::kArray || dt == DataType::kMap;
}

// Types valid for use in a user-defined row key schema.
constexpr bool is_key_compatible(DataType dt) {
    // None, compound containers, and private types cannot be key columns.
    return dt != DataType::kNone && dt != DataType::kArray &&
           dt != DataType::kMap && dt != DataType::kDataBlock &&
           dt != DataType::kIndexBlock;
}

// Private types used only in index block entries.
constexpr bool is_private_type(DataType dt) {
    return dt == DataType::kDataBlock || dt == DataType::kIndexBlock;
}

// =============================================================================
// Wire size: the fixed byte count for fixed-size types.
// Returns 0 for variable-length and compound types.
// =============================================================================

constexpr size_t fixed_size_of(DataType dt) {
    switch (dt) {
    case DataType::kBool:
    case DataType::kInt8:
    case DataType::kUint8:       return 1;
    case DataType::kInt16:
    case DataType::kUint16:      return 2;
    case DataType::kInt32:
    case DataType::kUint32:
    case DataType::kFloat:       return 4;
    case DataType::kInt64:
    case DataType::kUint64:
    case DataType::kDouble:      return 8;
    case DataType::kLongDouble:  return 16;
    default:                     return 0;
    }
}

// =============================================================================
// String representation (constexpr in C++17 via string_view).
// =============================================================================

constexpr std::string_view data_type_name(DataType dt) {
    switch (dt) {
    case DataType::kNone:        return "None";
    case DataType::kBool:        return "Bool";
    case DataType::kInt8:        return "Int8";
    case DataType::kUint8:       return "Uint8";
    case DataType::kInt16:       return "Int16";
    case DataType::kUint16:      return "Uint16";
    case DataType::kInt32:       return "Int32";
    case DataType::kUint32:      return "Uint32";
    case DataType::kInt64:       return "Int64";
    case DataType::kUint64:      return "Uint64";
    case DataType::kFloat:       return "Float";
    case DataType::kDouble:      return "Double";
    case DataType::kLongDouble:  return "LongDouble";
    case DataType::kTime:        return "Time";
    case DataType::kVersion:     return "Version";
    case DataType::kString:      return "String";
    case DataType::kU16String:   return "U16String";
    case DataType::kU32String:   return "U32String";
    case DataType::kBinary:      return "Binary";
    case DataType::kArray:       return "Array";
    case DataType::kMap:         return "Map";
    case DataType::kDataBlock:   return "DataBlock";
    case DataType::kIndexBlock:  return "IndexBlock";
    }
    return "Unknown";
}

// Stream output for diagnostics and test failure messages.
inline std::ostream& operator<<(std::ostream& os, DataType dt) {
    return os << data_type_name(dt);
}

} // namespace pl::sstv2::types
