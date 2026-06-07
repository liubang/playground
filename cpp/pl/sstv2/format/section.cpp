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
// Created: 2026/06/06 14:11

#include "cpp/pl/sstv2/format/section.h"

#include <cstring>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/checksum.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/varint.h"

namespace pl::sstv2::format {
namespace {

using types::DataType;
using types::MapStorage;
using types::Value;

void append_header(SectionMagic magic, uint64_t checksum, std::string* dst) {
    codec::append_fixed32(dst, static_cast<uint32_t>(magic));
    codec::append_fixed64(dst, checksum);
}

void append_value(const Value& value, std::string* dst) {
    codec::append_fixed8(dst, static_cast<uint8_t>(value.type()));
    switch (value.type()) {
        case DataType::kBool:
            codec::append_fixed8(dst, value.as_bool() ? 1 : 0);
            break;
        case DataType::kInt8:
            codec::append_fixed8(dst, static_cast<uint8_t>(value.as_int8()));
            break;
        case DataType::kUint8:
            codec::append_fixed8(dst, value.as_uint8());
            break;
        case DataType::kInt16:
            codec::append_fixed16(dst, static_cast<uint16_t>(value.as_int16()));
            break;
        case DataType::kUint16:
            codec::append_fixed16(dst, value.as_uint16());
            break;
        case DataType::kInt32:
            codec::append_fixed32(dst, static_cast<uint32_t>(value.as_int32()));
            break;
        case DataType::kUint32:
            codec::append_fixed32(dst, value.as_uint32());
            break;
        case DataType::kInt64:
            codec::append_fixed64(dst, static_cast<uint64_t>(value.as_int64()));
            break;
        case DataType::kUint64:
            codec::append_fixed64(dst, value.as_uint64());
            break;
        case DataType::kFloat: {
            const float scalar = value.as_float();
            uint32_t bits;
            std::memcpy(&bits, &scalar, sizeof(bits));
            codec::append_fixed32(dst, bits);
            break;
        }
        case DataType::kDouble: {
            const double scalar = value.as_double();
            uint64_t bits;
            std::memcpy(&bits, &scalar, sizeof(bits));
            codec::append_fixed64(dst, bits);
            break;
        }
        case DataType::kLongDouble:
            dst->append(reinterpret_cast<const char*>(value.as_long_double().data), 16);
            break;
        case DataType::kTime:
            codec::append_fixed64(dst, static_cast<uint64_t>(value.as_time().seconds));
            codec::append_fixed32(dst, value.as_time().nanoseconds);
            break;
        case DataType::kVersion:
            codec::append_fixed64(dst, value.as_version().major);
            codec::append_fixed64(dst, value.as_version().minor);
            break;
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary: {
            const std::string_view bytes = value.as_string();
            codec::encode_varint(bytes.size(), dst);
            dst->append(bytes);
            break;
        }
        default:
            break;
    }
}

absl::StatusOr<Value> read_value(std::string_view input, size_t* pos) {
    if (*pos >= input.size()) {
        return absl::InvalidArgumentError("missing section value type");
    }
    const auto type = static_cast<DataType>(static_cast<uint8_t>(input[*pos]));
    ++*pos;

    auto need = [&](size_t n) -> absl::Status {
        if (input.size() - *pos < n) {
            return absl::InvalidArgumentError("truncated section value");
        }
        return absl::OkStatus();
    };
    auto read32 = [&]() {
        const uint32_t value = codec::read_fixed32(input, *pos);
        *pos += 4;
        return value;
    };
    auto read16 = [&]() {
        const uint16_t value = codec::read_fixed16(input, *pos);
        *pos += 2;
        return value;
    };
    auto read64 = [&]() {
        const uint64_t value = codec::read_fixed64(input, *pos);
        *pos += 8;
        return value;
    };

    switch (type) {
        case DataType::kBool:
            if (auto status = need(1); !status.ok())
                return status;
            return Value::make<DataType::kBool>(input[(*pos)++] != 0);
        case DataType::kInt8:
            if (auto status = need(1); !status.ok())
                return status;
            return Value::make<DataType::kInt8>(static_cast<int8_t>(input[(*pos)++]));
        case DataType::kUint8:
            if (auto status = need(1); !status.ok())
                return status;
            return Value::make<DataType::kUint8>(static_cast<uint8_t>(input[(*pos)++]));
        case DataType::kInt16:
            if (auto status = need(2); !status.ok())
                return status;
            return Value::make<DataType::kInt16>(static_cast<int16_t>(read16()));
        case DataType::kUint16:
            if (auto status = need(2); !status.ok())
                return status;
            return Value::make<DataType::kUint16>(read16());
        case DataType::kInt32:
            if (auto status = need(4); !status.ok())
                return status;
            return Value::make<DataType::kInt32>(static_cast<int32_t>(read32()));
        case DataType::kUint32:
            if (auto status = need(4); !status.ok())
                return status;
            return Value::make<DataType::kUint32>(read32());
        case DataType::kInt64:
            if (auto status = need(8); !status.ok())
                return status;
            return Value::make<DataType::kInt64>(static_cast<int64_t>(read64()));
        case DataType::kUint64:
            if (auto status = need(8); !status.ok())
                return status;
            return Value::make<DataType::kUint64>(read64());
        case DataType::kFloat: {
            if (auto status = need(4); !status.ok())
                return status;
            const uint32_t bits = read32();
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return Value::make<DataType::kFloat>(value);
        }
        case DataType::kDouble: {
            if (auto status = need(8); !status.ok())
                return status;
            const uint64_t bits = read64();
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            return Value::make<DataType::kDouble>(value);
        }
        case DataType::kLongDouble: {
            if (auto status = need(16); !status.ok())
                return status;
            types::LongDouble value;
            std::memcpy(value.data, input.data() + *pos, sizeof(value.data));
            *pos += sizeof(value.data);
            return Value::make<DataType::kLongDouble>(value);
        }
        case DataType::kTime:
            if (auto status = need(12); !status.ok())
                return status;
            return Value::make<DataType::kTime>(
                types::Time{.seconds = static_cast<int64_t>(read64()), .nanoseconds = read32()});
        case DataType::kVersion:
            if (auto status = need(16); !status.ok())
                return status;
            return Value::make<DataType::kVersion>(
                types::Version{.major = read64(), .minor = read64()});
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary: {
            uint64_t len = 0;
            const size_t n = codec::decode_varint(
                reinterpret_cast<const uint8_t*>(input.data() + *pos), input.size() - *pos, &len);
            if (n == 0) {
                return absl::InvalidArgumentError("bad section string length");
            }
            *pos += n;
            if (input.size() - *pos < len) {
                return absl::InvalidArgumentError("truncated section string");
            }
            std::string bytes(input.data() + *pos, static_cast<size_t>(len));
            *pos += static_cast<size_t>(len);
            switch (type) {
                case DataType::kString:
                    return Value::make<DataType::kString>(std::move(bytes));
                case DataType::kU16String:
                    return Value::make<DataType::kU16String>(std::move(bytes));
                case DataType::kU32String:
                    return Value::make<DataType::kU32String>(std::move(bytes));
                default:
                    return Value::make<DataType::kBinary>(std::move(bytes));
            }
        }
        default:
            return absl::InvalidArgumentError(
                absl::StrCat("unsupported section value type ", types::data_type_name(type)));
    }
}

} // namespace

std::string encode_section(SectionMagic magic, const SectionMap& entries) {
    if (entries.type() != DataType::kMap) {
        return {};
    }
    std::string encoded;
    append_header(magic, 0, &encoded);
    codec::encode_varint(entries.as_map().size(), &encoded);
    for (const auto& [key_value, value] : entries.as_map()) {
        if (key_value.type() != DataType::kString) {
            return {};
        }
        const std::string_view key = key_value.as_string();
        codec::encode_varint(key.size(), &encoded);
        encoded.append(key);
        append_value(value, &encoded);
    }

    const uint64_t checksum = codec::crc32c_u64(encoded);
    std::string header;
    append_header(magic, checksum, &header);
    encoded.replace(0, header.size(), header);
    return encoded;
}

absl::StatusOr<Section> decode_section(std::string_view input, SectionMagic expected) {
    if (input.size() < 12) {
        return absl::InvalidArgumentError("section is shorter than header");
    }
    const auto magic = static_cast<SectionMagic>(codec::read_fixed32(input, 0));
    const uint64_t checksum = codec::read_fixed64(input, 4);
    if (magic != expected) {
        return absl::InvalidArgumentError("section magic mismatch");
    }

    if (codec::crc32c_u64_with_zeroed_range(input, 4, 8) != checksum) {
        return absl::InvalidArgumentError("section checksum mismatch");
    }

    size_t pos = 12;
    uint64_t count = 0;
    const size_t n = codec::decode_varint(
        reinterpret_cast<const uint8_t*>(input.data() + pos), input.size() - pos, &count);
    if (n == 0) {
        return absl::InvalidArgumentError("bad section entry count");
    }
    pos += n;

    SectionEntries entries;
    entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key_len = 0;
        const size_t key_n = codec::decode_varint(
            reinterpret_cast<const uint8_t*>(input.data() + pos), input.size() - pos, &key_len);
        if (key_n == 0) {
            return absl::InvalidArgumentError("bad section key length");
        }
        pos += key_n;
        if (input.size() - pos < key_len) {
            return absl::InvalidArgumentError("truncated section key");
        }
        std::string key(input.data() + pos, static_cast<size_t>(key_len));
        pos += static_cast<size_t>(key_len);
        auto value = read_value(input, &pos);
        if (!value.ok()) {
            return value.status();
        }
        entries.emplace_back(std::move(key), std::move(*value));
    }
    if (pos != input.size()) {
        return absl::InvalidArgumentError("trailing bytes after section entries");
    }
    Section section{
        .magic = magic, .checksum = checksum, .entries = make_section_map(std::move(entries))};
    return section;
}

SectionMap make_section_map(SectionEntries entries) {
    MapStorage map_entries;
    map_entries.reserve(entries.size());
    for (auto& [key, value] : entries) {
        map_entries.emplace_back(Value::make<DataType::kString>(std::move(key)), std::move(value));
    }
    return Value::make_map(std::move(map_entries));
}

const Value* find_section_value(const SectionMap& entries, std::string_view key) {
    if (entries.type() != DataType::kMap) {
        return nullptr;
    }
    for (const auto& [entry_key, value] : entries.as_map()) {
        if (entry_key.type() == DataType::kString && entry_key.as_string() == key) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace pl::sstv2::format
