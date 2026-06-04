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

#include "cpp/pl/sstv2/types/variant.h"

#include <cassert>
#include <cstring>
#include <new>

#include "absl/status/status.h"

namespace pl::sstv2::types {

namespace {

// Encodes a uint32 as a varint into out, appending bytes.
void encode_varint32(uint32_t value, std::string& out) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

// Decodes a varint from data, returns {value, bytes_consumed}.
// Returns {0, 0} on error (insufficient data or overflow).
std::pair<uint32_t, size_t> decode_varint32(std::span<const std::byte> data) {
    uint32_t result = 0;
    size_t shift = 0;
    for (size_t i = 0; i < data.size() && i < 5; ++i) {
        auto byte = static_cast<uint8_t>(data[i]);
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return {result, i + 1};
        }
        shift += 7;
    }
    return {0, 0};
}

// Appends value as little-endian bytes.
template <typename T> void append_le(const T& value, std::string& out) {
    // Rely on memcpy for type-punning safety; compilers optimize this well.
    char buf[sizeof(T)];
    std::memcpy(buf, &value, sizeof(T));
    out.append(buf, sizeof(T));
}

// Reads a little-endian value from data.
template <typename T> T read_le(std::span<const std::byte> data) {
    T value;
    std::memcpy(&value, data.data(), sizeof(T));
    return value;
}

} // namespace

// --- Lifecycle ---

bool Variant::uses_string_storage() const {
    return is_variable_size(type_) || type_ == DataType::kLongDouble;
}

void Variant::destroy() {
    if (uses_string_storage()) {
        str_val_.~basic_string();
    }
}

void Variant::copy_from(const Variant& other) {
    type_ = other.type_;
    if (other.uses_string_storage()) {
        new (&str_val_) std::string(other.str_val_);
    } else {
        // Copy the largest union member to cover all fixed-size cases.
        uint_val_ = other.uint_val_;
    }
}

void Variant::move_from(Variant&& other) noexcept {
    type_ = other.type_;
    if (other.uses_string_storage()) {
        new (&str_val_) std::string(std::move(other.str_val_));
    } else {
        uint_val_ = other.uint_val_;
    }
}

Variant::~Variant() {
    destroy();
}

Variant::Variant(const Variant& other) {
    copy_from(other);
}

Variant& Variant::operator=(const Variant& other) {
    if (this != &other) {
        destroy();
        copy_from(other);
    }
    return *this;
}

Variant::Variant(Variant&& other) noexcept {
    move_from(std::move(other));
}

Variant& Variant::operator=(Variant&& other) noexcept {
    if (this != &other) {
        destroy();
        move_from(std::move(other));
    }
    return *this;
}

// --- Factory methods ---

Variant Variant::none() {
    Variant v;
    v.type_ = DataType::kNone;
    v.int_val_ = 0;
    return v;
}

Variant Variant::boolean(bool b) {
    Variant v;
    v.type_ = DataType::kBool;
    v.bool_val_ = b;
    return v;
}

Variant Variant::int8(int8_t val) {
    Variant v;
    v.type_ = DataType::kInt8;
    v.int_val_ = val;
    return v;
}

Variant Variant::int16(int16_t val) {
    Variant v;
    v.type_ = DataType::kInt16;
    v.int_val_ = val;
    return v;
}

Variant Variant::int32(int32_t val) {
    Variant v;
    v.type_ = DataType::kInt32;
    v.int_val_ = val;
    return v;
}

Variant Variant::int64(int64_t val) {
    Variant v;
    v.type_ = DataType::kInt64;
    v.int_val_ = val;
    return v;
}

Variant Variant::uint8(uint8_t val) {
    Variant v;
    v.type_ = DataType::kUint8;
    v.uint_val_ = val;
    return v;
}

Variant Variant::uint16(uint16_t val) {
    Variant v;
    v.type_ = DataType::kUint16;
    v.uint_val_ = val;
    return v;
}

Variant Variant::uint32(uint32_t val) {
    Variant v;
    v.type_ = DataType::kUint32;
    v.uint_val_ = val;
    return v;
}

Variant Variant::uint64(uint64_t val) {
    Variant v;
    v.type_ = DataType::kUint64;
    v.uint_val_ = val;
    return v;
}

Variant Variant::float32(float val) {
    Variant v;
    v.type_ = DataType::kFloat;
    v.double_val_ = val;
    return v;
}

Variant Variant::float64(double val) {
    Variant v;
    v.type_ = DataType::kDouble;
    v.double_val_ = val;
    return v;
}

Variant Variant::time(int64_t microseconds) {
    Variant v;
    v.type_ = DataType::kTime;
    v.int_val_ = microseconds;
    return v;
}

Variant Variant::version(uint64_t val) {
    Variant v;
    v.type_ = DataType::kVersion;
    v.uint_val_ = val;
    return v;
}

Variant Variant::string(std::string_view s) {
    Variant v;
    v.type_ = DataType::kString;
    new (&v.str_val_) std::string(s);
    return v;
}

Variant Variant::binary(std::span<const std::byte> b) {
    Variant v;
    v.type_ = DataType::kBinary;
    new (&v.str_val_) std::string(reinterpret_cast<const char*>(b.data()), b.size());
    return v;
}

// --- Accessors ---

DataType Variant::type() const {
    return type_;
}

bool Variant::is_none() const {
    return type_ == DataType::kNone;
}

bool Variant::as_bool() const {
    assert(type_ == DataType::kBool);
    return bool_val_;
}

int64_t Variant::as_int() const {
    assert(is_signed_integer(type_) || type_ == DataType::kTime);
    return int_val_;
}

uint64_t Variant::as_uint() const {
    assert(is_unsigned_integer(type_) || type_ == DataType::kVersion);
    return uint_val_;
}

double Variant::as_float() const {
    assert(is_floating_point(type_));
    return double_val_;
}

std::string_view Variant::as_string() const {
    assert(is_string_type(type_));
    return str_val_;
}

std::span<const std::byte> Variant::as_binary() const {
    assert(type_ == DataType::kBinary);
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(str_val_.data()),
                                      str_val_.size());
}

// --- Comparison ---

// Same-type comparison by value; cross-type comparison by DataType enum value.
std::strong_ordering Variant::operator<=>(const Variant& other) const {
    if (type_ != other.type_) {
        return static_cast<uint8_t>(type_) <=> static_cast<uint8_t>(other.type_);
    }

    switch (type_) {
        case DataType::kNone:
            return std::strong_ordering::equal;
        case DataType::kBool:
            return static_cast<int>(bool_val_) <=> static_cast<int>(other.bool_val_);
        case DataType::kInt8:
        case DataType::kInt16:
        case DataType::kInt32:
        case DataType::kInt64:
        case DataType::kTime:
            return int_val_ <=> other.int_val_;
        case DataType::kUint8:
        case DataType::kUint16:
        case DataType::kUint32:
        case DataType::kUint64:
        case DataType::kVersion:
            return uint_val_ <=> other.uint_val_;
        case DataType::kFloat:
        case DataType::kDouble: {
            // strong_ordering requires total ordering; treat as bit-comparable
            // after normalizing NaN. For storage purposes we use a simple approach.
            if (double_val_ < other.double_val_)
                return std::strong_ordering::less;
            if (double_val_ > other.double_val_)
                return std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }
        case DataType::kLongDouble:
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary:
            return str_val_ <=> other.str_val_;
        default:
            // Compound/private types: compare by type only
            return std::strong_ordering::equal;
    }
}

bool Variant::operator==(const Variant& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

// --- Serialization ---

void Variant::encode_to(std::string& out) const {
    switch (type_) {
        case DataType::kNone:
            break;
        case DataType::kBool:
            out.push_back(bool_val_ ? '\x01' : '\x00');
            break;
        case DataType::kInt8:
            append_le(static_cast<int8_t>(int_val_), out);
            break;
        case DataType::kInt16:
            append_le(static_cast<int16_t>(int_val_), out);
            break;
        case DataType::kInt32:
            append_le(static_cast<int32_t>(int_val_), out);
            break;
        case DataType::kInt64:
        case DataType::kTime:
            append_le(int_val_, out);
            break;
        case DataType::kUint8:
            append_le(static_cast<uint8_t>(uint_val_), out);
            break;
        case DataType::kUint16:
            append_le(static_cast<uint16_t>(uint_val_), out);
            break;
        case DataType::kUint32:
            append_le(static_cast<uint32_t>(uint_val_), out);
            break;
        case DataType::kUint64:
        case DataType::kVersion:
            append_le(uint_val_, out);
            break;
        case DataType::kFloat:
            append_le(static_cast<float>(double_val_), out);
            break;
        case DataType::kDouble:
            append_le(double_val_, out);
            break;
        case DataType::kLongDouble:
            // Stored as raw bytes in str_val_
            encode_varint32(static_cast<uint32_t>(str_val_.size()), out);
            out.append(str_val_);
            break;
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary:
            encode_varint32(static_cast<uint32_t>(str_val_.size()), out);
            out.append(str_val_);
            break;
        default:
            break;
    }
}

absl::StatusOr<Variant> Variant::decode_from(DataType type, std::span<const std::byte> data) {
    switch (type) {
        case DataType::kNone:
            return none();

        case DataType::kBool:
            if (data.size() < 1)
                return absl::InvalidArgumentError("insufficient data for Bool");
            return boolean(static_cast<uint8_t>(data[0]) != 0);

        case DataType::kInt8:
            if (data.size() < 1)
                return absl::InvalidArgumentError("insufficient data for Int8");
            return int8(read_le<int8_t>(data));

        case DataType::kInt16:
            if (data.size() < 2)
                return absl::InvalidArgumentError("insufficient data for Int16");
            return int16(read_le<int16_t>(data));

        case DataType::kInt32:
            if (data.size() < 4)
                return absl::InvalidArgumentError("insufficient data for Int32");
            return int32(read_le<int32_t>(data));

        case DataType::kInt64:
            if (data.size() < 8)
                return absl::InvalidArgumentError("insufficient data for Int64");
            return int64(read_le<int64_t>(data));

        case DataType::kTime:
            if (data.size() < 8)
                return absl::InvalidArgumentError("insufficient data for Time");
            return time(read_le<int64_t>(data));

        case DataType::kUint8:
            if (data.size() < 1)
                return absl::InvalidArgumentError("insufficient data for Uint8");
            return uint8(read_le<uint8_t>(data));

        case DataType::kUint16:
            if (data.size() < 2)
                return absl::InvalidArgumentError("insufficient data for Uint16");
            return uint16(read_le<uint16_t>(data));

        case DataType::kUint32:
            if (data.size() < 4)
                return absl::InvalidArgumentError("insufficient data for Uint32");
            return uint32(read_le<uint32_t>(data));

        case DataType::kUint64:
            if (data.size() < 8)
                return absl::InvalidArgumentError("insufficient data for Uint64");
            return uint64(read_le<uint64_t>(data));

        case DataType::kVersion:
            if (data.size() < 8)
                return absl::InvalidArgumentError("insufficient data for Version");
            return version(read_le<uint64_t>(data));

        case DataType::kFloat:
            if (data.size() < 4)
                return absl::InvalidArgumentError("insufficient data for Float");
            return float32(read_le<float>(data));

        case DataType::kDouble:
            if (data.size() < 8)
                return absl::InvalidArgumentError("insufficient data for Double");
            return float64(read_le<double>(data));

        case DataType::kLongDouble:
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary: {
            auto [len, consumed] = decode_varint32(data);
            if (consumed == 0)
                return absl::InvalidArgumentError("failed to decode varint length");
            if (data.size() < consumed + len)
                return absl::InvalidArgumentError("insufficient data for variable-size");
            auto payload = data.subspan(consumed, len);
            if (type == DataType::kBinary) {
                return binary(payload);
            }
            // String types (and LongDouble stored as raw bytes)
            std::string_view sv(reinterpret_cast<const char*>(payload.data()), payload.size());
            if (type == DataType::kLongDouble) {
                // Reconstruct using string storage
                Variant v;
                v.type_ = DataType::kLongDouble;
                new (&v.str_val_) std::string(sv);
                return v;
            }
            Variant v;
            v.type_ = type;
            new (&v.str_val_) std::string(sv);
            return v;
        }

        default:
            return absl::InvalidArgumentError("unsupported DataType for decode");
    }
}

} // namespace pl::sstv2::types
