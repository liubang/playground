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

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pl::sstv2::types {

enum class DataType : uint8_t {
    // 空类型
    kNone = 0,

    // 布尔
    kBool = 1,

    // 有符号整数
    kInt8 = 2,
    kInt16 = 4,
    kInt32 = 6,
    kInt64 = 8,

    // 无符号整数
    kUint8 = 3,
    kUint16 = 5,
    kUint32 = 7,
    kUint64 = 9,

    // 浮点数
    kFloat = 10,
    kDouble = 11,
    kLongDouble = 12,

    // 时间类型
    kTime = 13,
    kVersion = 14,

    // 字符串类型（变长）
    kString = 15,
    kU16String = 16,
    kU32String = 17,
    kBinary = 18,

    // 复合类型
    kArray = 19,
    kMap = 20,

    // 私有类型（仅内部使用，不出现在用户 Schema 中）
    kDataBlock = 21,
    kIndexBlock = 22,
};

// === 分类函数 ===

constexpr bool is_none(DataType t) {
    return t == DataType::kNone;
}

constexpr bool is_bool(DataType t) {
    return t == DataType::kBool;
}

constexpr bool is_signed_integer(DataType t) {
    return t == DataType::kInt8 || t == DataType::kInt16 || t == DataType::kInt32 ||
           t == DataType::kInt64;
}

constexpr bool is_unsigned_integer(DataType t) {
    return t == DataType::kUint8 || t == DataType::kUint16 || t == DataType::kUint32 ||
           t == DataType::kUint64;
}

constexpr bool is_integral(DataType t) {
    return is_signed_integer(t) || is_unsigned_integer(t);
}

constexpr bool is_floating_point(DataType t) {
    return t == DataType::kFloat || t == DataType::kDouble || t == DataType::kLongDouble;
}

constexpr bool is_fixed_size(DataType t) {
    return is_bool(t) || is_integral(t) || is_floating_point(t) || t == DataType::kTime ||
           t == DataType::kVersion;
}

constexpr bool is_variable_size(DataType t) {
    return t == DataType::kString || t == DataType::kU16String || t == DataType::kU32String ||
           t == DataType::kBinary;
}

constexpr bool is_string_type(DataType t) {
    return t == DataType::kString || t == DataType::kU16String || t == DataType::kU32String;
}

constexpr bool is_compound(DataType t) {
    return t == DataType::kArray || t == DataType::kMap;
}

constexpr bool is_private(DataType t) {
    return t == DataType::kDataBlock || t == DataType::kIndexBlock;
}

constexpr bool is_public(DataType t) {
    return !is_none(t) && !is_private(t);
}

// Returns byte count for fixed-size types, 0 for variable-size/compound types.
constexpr size_t fixed_size_in_bytes(DataType t) {
    switch (t) {
        case DataType::kBool:
            return 1;
        case DataType::kInt8:
        case DataType::kUint8:
            return 1;
        case DataType::kInt16:
        case DataType::kUint16:
            return 2;
        case DataType::kInt32:
        case DataType::kUint32:
        case DataType::kFloat:
            return 4;
        case DataType::kInt64:
        case DataType::kUint64:
        case DataType::kDouble:
        case DataType::kTime:
        case DataType::kVersion:
            return 8;
        case DataType::kLongDouble:
            return 16;
        default:
            return 0;
    }
}

constexpr std::string_view data_type_name(DataType t) {
    switch (t) {
        case DataType::kNone:
            return "None";
        case DataType::kBool:
            return "Bool";
        case DataType::kInt8:
            return "Int8";
        case DataType::kInt16:
            return "Int16";
        case DataType::kInt32:
            return "Int32";
        case DataType::kInt64:
            return "Int64";
        case DataType::kUint8:
            return "Uint8";
        case DataType::kUint16:
            return "Uint16";
        case DataType::kUint32:
            return "Uint32";
        case DataType::kUint64:
            return "Uint64";
        case DataType::kFloat:
            return "Float";
        case DataType::kDouble:
            return "Double";
        case DataType::kLongDouble:
            return "LongDouble";
        case DataType::kTime:
            return "Time";
        case DataType::kVersion:
            return "Version";
        case DataType::kString:
            return "String";
        case DataType::kU16String:
            return "U16String";
        case DataType::kU32String:
            return "U32String";
        case DataType::kBinary:
            return "Binary";
        case DataType::kArray:
            return "Array";
        case DataType::kMap:
            return "Map";
        case DataType::kDataBlock:
            return "DataBlock";
        case DataType::kIndexBlock:
            return "IndexBlock";
    }
    return "Unknown";
}

} // namespace pl::sstv2::types
