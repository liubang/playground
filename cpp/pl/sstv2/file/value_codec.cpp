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
// Created: 2026/06/06 14:16

#include "cpp/pl/sstv2/file/value_codec.h"

#include <cstring>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/varint.h"

namespace pl::sstv2::file {
namespace {

using types::ArrayStorage;
using types::DataType;
using types::MapStorage;
using types::Value;

template <typename T> void append_bits(std::string* dst, const T& value) {
    dst->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void append_zigzag(int64_t value, std::string* dst) {
    codec::encode_varint(codec::zigzag_encode(value), dst);
}

absl::Status encode_variant(const Value& value, std::string* dst) {
    auto payload = encode_value(value);
    if (!payload.ok())
        return payload.status();
    codec::append_fixed8(dst, static_cast<uint8_t>(value.type()));
    codec::encode_varint(payload->size(), dst);
    dst->append(*payload);
    return absl::OkStatus();
}

absl::StatusOr<uint64_t> read_varint(std::string_view bytes, size_t* pos) {
    uint64_t value = 0;
    const size_t n = codec::decode_varint(
        reinterpret_cast<const uint8_t*>(bytes.data() + *pos), bytes.size() - *pos, &value);
    if (n == 0) {
        return absl::InvalidArgumentError("bad value varint");
    }
    *pos += n;
    return value;
}

absl::Status require_end(std::string_view bytes, size_t pos) {
    if (pos != bytes.size()) {
        return absl::InvalidArgumentError("trailing bytes in encoded value");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> decode_variant(std::string_view bytes, size_t* pos) {
    if (*pos >= bytes.size()) {
        return absl::InvalidArgumentError("missing variant type");
    }
    const auto type = static_cast<DataType>(static_cast<uint8_t>(bytes[(*pos)++]));
    auto len = read_varint(bytes, pos);
    if (!len.ok())
        return len.status();
    if (bytes.size() - *pos < *len) {
        return absl::InvalidArgumentError("truncated variant payload");
    }
    auto value = decode_value(type, bytes.substr(*pos, static_cast<size_t>(*len)));
    *pos += static_cast<size_t>(*len);
    return value;
}

} // namespace

absl::Status encode_value(const Value& value, std::string* dst) {
    switch (value.type()) {
        case DataType::kNone:
            return absl::OkStatus();
        case DataType::kBool:
            codec::append_fixed8(dst, value.as_bool() ? 1 : 0);
            return absl::OkStatus();
        case DataType::kInt8:
            codec::append_fixed8(dst, static_cast<uint8_t>(value.as_int8()));
            return absl::OkStatus();
        case DataType::kUint8:
            codec::append_fixed8(dst, value.as_uint8());
            return absl::OkStatus();
        case DataType::kInt16:
            append_zigzag(value.as_int16(), dst);
            return absl::OkStatus();
        case DataType::kUint16:
            codec::encode_varint(value.as_uint16(), dst);
            return absl::OkStatus();
        case DataType::kInt32:
            append_zigzag(value.as_int32(), dst);
            return absl::OkStatus();
        case DataType::kUint32:
            codec::encode_varint(value.as_uint32(), dst);
            return absl::OkStatus();
        case DataType::kInt64:
            append_zigzag(value.as_int64(), dst);
            return absl::OkStatus();
        case DataType::kUint64:
            codec::encode_varint(value.as_uint64(), dst);
            return absl::OkStatus();
        case DataType::kFloat:
            append_bits(dst, value.as_float());
            return absl::OkStatus();
        case DataType::kDouble:
            append_bits(dst, value.as_double());
            return absl::OkStatus();
        case DataType::kLongDouble:
            dst->append(reinterpret_cast<const char*>(value.as_long_double().data), 16);
            return absl::OkStatus();
        case DataType::kTime:
            append_zigzag(value.as_time().seconds, dst);
            codec::encode_varint(value.as_time().nanoseconds, dst);
            return absl::OkStatus();
        case DataType::kVersion:
            codec::encode_varint(value.as_version().major, dst);
            codec::encode_varint(value.as_version().minor, dst);
            return absl::OkStatus();
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary:
            dst->append(value.as_string());
            return absl::OkStatus();
        case DataType::kArray:
            codec::encode_varint(value.as_array().size(), dst);
            for (const auto& element : value.as_array()) {
                auto status = encode_variant(element, dst);
                if (!status.ok())
                    return status;
            }
            return absl::OkStatus();
        case DataType::kMap:
            codec::encode_varint(value.as_map().size(), dst);
            for (const auto& [key, item] : value.as_map()) {
                auto key_status = encode_variant(key, dst);
                if (!key_status.ok())
                    return key_status;
                auto value_status = encode_variant(item, dst);
                if (!value_status.ok())
                    return value_status;
            }
            return absl::OkStatus();
        default:
            return absl::InvalidArgumentError(absl::StrCat("cannot encode private value type ",
                                                           types::data_type_name(value.type())));
    }
}

absl::StatusOr<std::string> encode_value(const Value& value) {
    std::string encoded;
    auto status = encode_value(value, &encoded);
    if (!status.ok())
        return status;
    return encoded;
}

absl::StatusOr<Value> decode_value(DataType type, std::string_view bytes) {
    size_t pos = 0;
    switch (type) {
        case DataType::kNone:
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value{};
        case DataType::kBool:
            if (bytes.size() != 1)
                return absl::InvalidArgumentError("bad Bool value length");
            return Value::make<DataType::kBool>(bytes[0] != 0);
        case DataType::kInt8:
            if (bytes.size() != 1)
                return absl::InvalidArgumentError("bad Int8 value length");
            return Value::make<DataType::kInt8>(static_cast<int8_t>(bytes[0]));
        case DataType::kUint8:
            if (bytes.size() != 1)
                return absl::InvalidArgumentError("bad Uint8 value length");
            return Value::make<DataType::kUint8>(static_cast<uint8_t>(bytes[0]));
        case DataType::kInt16: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kInt16>(static_cast<int16_t>(codec::zigzag_decode(*v)));
        }
        case DataType::kUint16: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kUint16>(static_cast<uint16_t>(*v));
        }
        case DataType::kInt32: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kInt32>(static_cast<int32_t>(codec::zigzag_decode(*v)));
        }
        case DataType::kUint32: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kUint32>(static_cast<uint32_t>(*v));
        }
        case DataType::kInt64: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kInt64>(codec::zigzag_decode(*v));
        }
        case DataType::kUint64: {
            auto v = read_varint(bytes, &pos);
            if (!v.ok())
                return v.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kUint64>(*v);
        }
        case DataType::kFloat: {
            if (bytes.size() != 4)
                return absl::InvalidArgumentError("bad Float value length");
            float value;
            std::memcpy(&value, bytes.data(), sizeof(value));
            return Value::make<DataType::kFloat>(value);
        }
        case DataType::kDouble: {
            if (bytes.size() != 8)
                return absl::InvalidArgumentError("bad Double value length");
            double value;
            std::memcpy(&value, bytes.data(), sizeof(value));
            return Value::make<DataType::kDouble>(value);
        }
        case DataType::kLongDouble: {
            if (bytes.size() != 16)
                return absl::InvalidArgumentError("bad LongDouble value length");
            types::LongDouble value;
            std::memcpy(value.data, bytes.data(), 16);
            return Value::make<DataType::kLongDouble>(value);
        }
        case DataType::kTime: {
            auto seconds = read_varint(bytes, &pos);
            if (!seconds.ok())
                return seconds.status();
            auto nanos = read_varint(bytes, &pos);
            if (!nanos.ok())
                return nanos.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kTime>(
                types::Time{.seconds = codec::zigzag_decode(*seconds),
                            .nanoseconds = static_cast<uint32_t>(*nanos)});
        }
        case DataType::kVersion: {
            auto major = read_varint(bytes, &pos);
            if (!major.ok())
                return major.status();
            auto minor = read_varint(bytes, &pos);
            if (!minor.ok())
                return minor.status();
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make<DataType::kVersion>(
                types::Version{.major = *major, .minor = *minor});
        }
        case DataType::kString:
            return Value::make<DataType::kString>(bytes);
        case DataType::kU16String:
            return Value::make<DataType::kU16String>(bytes);
        case DataType::kU32String:
            return Value::make<DataType::kU32String>(bytes);
        case DataType::kBinary:
            return Value::make<DataType::kBinary>(bytes);
        case DataType::kArray: {
            auto count = read_varint(bytes, &pos);
            if (!count.ok())
                return count.status();
            ArrayStorage array;
            array.reserve(static_cast<size_t>(*count));
            for (uint64_t i = 0; i < *count; ++i) {
                auto value = decode_variant(bytes, &pos);
                if (!value.ok())
                    return value.status();
                array.push_back(std::move(*value));
            }
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make_array(std::move(array));
        }
        case DataType::kMap: {
            auto count = read_varint(bytes, &pos);
            if (!count.ok())
                return count.status();
            MapStorage map;
            map.reserve(static_cast<size_t>(*count));
            for (uint64_t i = 0; i < *count; ++i) {
                auto key = decode_variant(bytes, &pos);
                if (!key.ok())
                    return key.status();
                auto value = decode_variant(bytes, &pos);
                if (!value.ok())
                    return value.status();
                map.emplace_back(std::move(*key), std::move(*value));
            }
            if (auto status = require_end(bytes, pos); !status.ok())
                return status;
            return Value::make_map(std::move(map));
        }
        default:
            return absl::InvalidArgumentError(
                absl::StrCat("cannot decode private value type ", types::data_type_name(type)));
    }
}

} // namespace pl::sstv2::file
