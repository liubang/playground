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
// Created: 2026/06/05 22:09

#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/comparable.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::codec {

namespace detail {

inline void encode_unsigned(uint64_t v, size_t bytes, bool desc, std::string* dst) {
    switch (bytes) {
        case 1:
            desc ? encode_uint8_desc(static_cast<uint8_t>(v), dst)
                 : encode_uint8(static_cast<uint8_t>(v), dst);
            break;
        case 2:
            desc ? encode_uint16_desc(static_cast<uint16_t>(v), dst)
                 : encode_uint16(static_cast<uint16_t>(v), dst);
            break;
        case 4:
            desc ? encode_uint32_desc(static_cast<uint32_t>(v), dst)
                 : encode_uint32(static_cast<uint32_t>(v), dst);
            break;
        default:
            desc ? encode_uint64_desc(v, dst) : encode_uint64(v, dst);
            break;
    }
}

inline void encode_signed(int64_t v, size_t bytes, bool desc, std::string* dst) {
    switch (bytes) {
        case 1:
            desc ? encode_int8_desc(static_cast<int8_t>(v), dst)
                 : encode_int8(static_cast<int8_t>(v), dst);
            break;
        case 2:
            desc ? encode_int16_desc(static_cast<int16_t>(v), dst)
                 : encode_int16(static_cast<int16_t>(v), dst);
            break;
        case 4:
            desc ? encode_int32_desc(static_cast<int32_t>(v), dst)
                 : encode_int32(static_cast<int32_t>(v), dst);
            break;
        default:
            desc ? encode_int64_desc(v, dst) : encode_int64(v, dst);
            break;
    }
}

} // namespace detail

inline absl::Status encode_value_comparable(const types::Value& value,
                                            types::DataType type,
                                            types::SortOrder order,
                                            std::string* dst);

inline absl::Status encode_array_comparable(const types::ArrayStorage& values,
                                            bool desc,
                                            std::string* dst) {
    detail::encode_unsigned(values.size(), sizeof(uint64_t), desc, dst);
    for (const auto& value : values) {
        detail::encode_unsigned(static_cast<uint8_t>(value.type()), sizeof(uint8_t), desc, dst);
        auto status = encode_value_comparable(value,
                                              value.type(),
                                              desc ? types::SortOrder::kDescending
                                                   : types::SortOrder::kAscending,
                                              dst);
        if (!status.ok())
            return status;
    }
    return absl::OkStatus();
}

inline absl::Status encode_map_comparable(const types::MapStorage& entries,
                                          bool desc,
                                          std::string* dst) {
    detail::encode_unsigned(entries.size(), sizeof(uint64_t), desc, dst);
    for (const auto& [key, value] : entries) {
        detail::encode_unsigned(static_cast<uint8_t>(key.type()), sizeof(uint8_t), desc, dst);
        auto status = encode_value_comparable(key,
                                              key.type(),
                                              desc ? types::SortOrder::kDescending
                                                   : types::SortOrder::kAscending,
                                              dst);
        if (!status.ok())
            return status;
        detail::encode_unsigned(static_cast<uint8_t>(value.type()), sizeof(uint8_t), desc, dst);
        status = encode_value_comparable(value,
                                         value.type(),
                                         desc ? types::SortOrder::kDescending
                                              : types::SortOrder::kAscending,
                                         dst);
        if (!status.ok())
            return status;
    }
    return absl::OkStatus();
}

inline absl::Status encode_value_comparable(const types::Value& value,
                                            types::DataType type,
                                            types::SortOrder order,
                                            std::string* dst) {
    if (value.type() != type) {
        return absl::InvalidArgumentError(absl::StrCat("value type mismatch: expected ",
                                                       types::data_type_name(type),
                                                       ", got ",
                                                       types::data_type_name(value.type())));
    }

    const bool desc = order == types::SortOrder::kDescending;
    switch (type) {
        case types::DataType::kBool:
            detail::encode_unsigned(value.as_bool() ? 1 : 0, 1, desc, dst);
            return absl::OkStatus();
        case types::DataType::kInt8:
            detail::encode_signed(value.as_int8(), 1, desc, dst);
            return absl::OkStatus();
        case types::DataType::kUint8:
            detail::encode_unsigned(value.as_uint8(), 1, desc, dst);
            return absl::OkStatus();
        case types::DataType::kInt16:
            detail::encode_signed(value.as_int16(), 2, desc, dst);
            return absl::OkStatus();
        case types::DataType::kUint16:
            detail::encode_unsigned(value.as_uint16(), 2, desc, dst);
            return absl::OkStatus();
        case types::DataType::kInt32:
            detail::encode_signed(value.as_int32(), 4, desc, dst);
            return absl::OkStatus();
        case types::DataType::kUint32:
            detail::encode_unsigned(value.as_uint32(), 4, desc, dst);
            return absl::OkStatus();
        case types::DataType::kInt64:
            detail::encode_signed(value.as_int64(), 8, desc, dst);
            return absl::OkStatus();
        case types::DataType::kUint64:
            detail::encode_unsigned(value.as_uint64(), 8, desc, dst);
            return absl::OkStatus();
        case types::DataType::kFloat:
            desc ? encode_float_desc(value.as_float(), dst) : encode_float(value.as_float(), dst);
            return absl::OkStatus();
        case types::DataType::kDouble:
            desc ? encode_double_desc(value.as_double(), dst)
                 : encode_double(value.as_double(), dst);
            return absl::OkStatus();
        case types::DataType::kLongDouble:
            desc ? encode_bytes_desc(
                       std::string_view(reinterpret_cast<const char*>(value.as_long_double().data),
                                        16),
                       dst)
                 : encode_bytes(std::string_view(
                                    reinterpret_cast<const char*>(value.as_long_double().data), 16),
                                dst);
            return absl::OkStatus();
        case types::DataType::kTime: {
            const auto& t = value.as_time();
            detail::encode_signed(t.seconds, 8, desc, dst);
            detail::encode_unsigned(t.nanoseconds, 4, desc, dst);
            return absl::OkStatus();
        }
        case types::DataType::kVersion: {
            const auto& v = value.as_version();
            detail::encode_unsigned(v.major, 8, desc, dst);
            detail::encode_unsigned(v.minor, 8, desc, dst);
            return absl::OkStatus();
        }
        case types::DataType::kString:
        case types::DataType::kU16String:
        case types::DataType::kU32String:
        case types::DataType::kBinary:
            desc ? encode_bytes_desc(value.as_string(), dst) : encode_bytes(value.as_string(), dst);
            return absl::OkStatus();
        case types::DataType::kArray:
            return encode_array_comparable(value.as_array(), desc, dst);
        case types::DataType::kMap:
            return encode_map_comparable(value.as_map(), desc, dst);
        default:
            return absl::InvalidArgumentError(
                absl::StrCat("type ", types::data_type_name(type), " is not comparable"));
    }
}

inline absl::Status encode_all_key(const types::InternalRow& row,
                                   types::InternalSchema::ConstRef schema,
                                   std::string* dst) {
    dst->clear();
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        auto status = encode_value_comparable(
            row.columns[i], schema->column_type(i), schema->column_order(i), dst);
        if (!status.ok())
            return status;
    }
    return absl::OkStatus();
}

} // namespace pl::sstv2::codec
