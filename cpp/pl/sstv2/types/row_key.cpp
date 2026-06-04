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

#include "cpp/pl/sstv2/types/row_key.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace pl::sstv2::types {

namespace {

// --- Memcomparable encoding helpers ---

// Encode unsigned integer in big-endian (memcomparable order).
template <typename T> void encode_uint_be(T value, std::string& out) {
    static_assert(std::is_unsigned_v<T>);
    for (int i = sizeof(T) - 1; i >= 0; --i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
    }
}

// Encode signed integer: flip sign bit then big-endian.
template <typename T> void encode_int_be(T value, std::string& out) {
    static_assert(std::is_signed_v<T>);
    using U = std::make_unsigned_t<T>;
    U u;
    std::memcpy(&u, &value, sizeof(T));
    // Flip the sign bit so that negative < positive in memcmp.
    u ^= (U{1} << (sizeof(T) * 8 - 1));
    encode_uint_be(u, out);
}

// Encode float: if negative flip all bits, else flip sign bit only.
void encode_float_be(float value, std::string& out) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    if (bits & 0x80000000u) {
        bits = ~bits;
    } else {
        bits ^= 0x80000000u;
    }
    encode_uint_be(bits, out);
}

// Encode double: if negative flip all bits, else flip sign bit only.
void encode_double_be(double value, std::string& out) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    if (bits & 0x8000000000000000ull) {
        bits = ~bits;
    } else {
        bits ^= 0x8000000000000000ull;
    }
    encode_uint_be(bits, out);
}

// Encode variable-length bytes (string/binary) using escaped encoding:
// Each group of 8 bytes is followed by a marker byte.
// Marker = 0xFF means group is full (8 bytes), 0x00..0x08 means last group has that many bytes.
// This gives memcomparable order and unambiguous termination.
void encode_bytes_comparable(std::string_view data, std::string& out) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t remaining = data.size() - pos;
        size_t group_len = std::min<size_t>(remaining, 8);
        out.append(data.data() + pos, group_len);
        if (group_len < 8) {
            // Pad with zeros.
            out.append(8 - group_len, '\0');
        }
        // Marker: 0xFF for full group, otherwise number of valid bytes in last group.
        if (remaining > 8) {
            out.push_back(static_cast<char>(0xFF));
        } else {
            out.push_back(static_cast<char>(group_len));
        }
        pos += 8;
    }
    if (data.empty()) {
        // Empty string: 8 zero bytes + marker 0.
        out.append(8, '\0');
        out.push_back('\0');
    }
}

// Encode bool: 0x00 = false, 0x01 = true.
void encode_bool(bool value, std::string& out) {
    out.push_back(value ? '\x01' : '\x00');
}

// Apply DESC: bitwise NOT of the encoded bytes for a column.
void apply_descending(std::string& out, size_t start_offset) {
    for (size_t i = start_offset; i < out.size(); ++i) {
        out[i] = static_cast<char>(~static_cast<unsigned char>(out[i]));
    }
}

// Encode one column value in memcomparable format.
void encode_column(const Variant& value, DataType type, std::string& out) {
    switch (type) {
        case DataType::kBool:
            encode_bool(value.as_bool(), out);
            break;
        case DataType::kInt8:
            encode_int_be(static_cast<int8_t>(value.as_int()), out);
            break;
        case DataType::kInt16:
            encode_int_be(static_cast<int16_t>(value.as_int()), out);
            break;
        case DataType::kInt32:
            encode_int_be(static_cast<int32_t>(value.as_int()), out);
            break;
        case DataType::kInt64:
        case DataType::kTime:
            encode_int_be(value.as_int(), out);
            break;
        case DataType::kUint8:
            encode_uint_be(static_cast<uint8_t>(value.as_uint()), out);
            break;
        case DataType::kUint16:
            encode_uint_be(static_cast<uint16_t>(value.as_uint()), out);
            break;
        case DataType::kUint32:
            encode_uint_be(static_cast<uint32_t>(value.as_uint()), out);
            break;
        case DataType::kUint64:
        case DataType::kVersion:
            encode_uint_be(value.as_uint(), out);
            break;
        case DataType::kFloat:
            encode_float_be(static_cast<float>(value.as_float()), out);
            break;
        case DataType::kDouble:
            encode_double_be(value.as_float(), out);
            break;
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
            encode_bytes_comparable(value.as_string(), out);
            break;
        case DataType::kBinary: {
            auto bin = value.as_binary();
            encode_bytes_comparable(
                std::string_view(reinterpret_cast<const char*>(bin.data()), bin.size()), out);
            break;
        }
        default:
            break;
    }
}

// --- Memcomparable decoding helpers ---

template <typename T> T decode_uint_be(std::string_view data, size_t& pos) {
    static_assert(std::is_unsigned_v<T>);
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = (value << 8) | static_cast<uint8_t>(data[pos + i]);
    }
    pos += sizeof(T);
    return value;
}

template <typename T> T decode_int_be(std::string_view data, size_t& pos) {
    static_assert(std::is_signed_v<T>);
    using U = std::make_unsigned_t<T>;
    U u = decode_uint_be<U>(data, pos);
    // Undo sign-bit flip.
    u ^= (U{1} << (sizeof(T) * 8 - 1));
    T result;
    std::memcpy(&result, &u, sizeof(T));
    return result;
}

float decode_float_be(std::string_view data, size_t& pos) {
    uint32_t bits = decode_uint_be<uint32_t>(data, pos);
    if (bits & 0x80000000u) {
        bits ^= 0x80000000u;
    } else {
        bits = ~bits;
    }
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

double decode_double_be(std::string_view data, size_t& pos) {
    uint64_t bits = decode_uint_be<uint64_t>(data, pos);
    if (bits & 0x8000000000000000ull) {
        bits ^= 0x8000000000000000ull;
    } else {
        bits = ~bits;
    }
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}

// Decode escaped bytes encoding. Returns the decoded string and advances pos.
std::string decode_bytes_comparable(std::string_view data, size_t& pos) {
    std::string result;
    while (pos + 9 <= data.size()) {
        auto marker = static_cast<uint8_t>(data[pos + 8]);
        if (marker == 0xFF) {
            // Full 8-byte group.
            result.append(data.data() + pos, 8);
            pos += 9;
        } else {
            // Last group: marker indicates valid bytes count.
            size_t valid = marker;
            result.append(data.data() + pos, valid);
            pos += 9;
            break;
        }
    }
    return result;
}

// Decode one column and return a Variant.
Variant decode_column(DataType type, std::string_view data, size_t& pos) {
    switch (type) {
        case DataType::kBool: {
            bool v = static_cast<uint8_t>(data[pos]) != 0;
            pos += 1;
            return Variant::boolean(v);
        }
        case DataType::kInt8:
            return Variant::int8(decode_int_be<int8_t>(data, pos));
        case DataType::kInt16:
            return Variant::int16(decode_int_be<int16_t>(data, pos));
        case DataType::kInt32:
            return Variant::int32(decode_int_be<int32_t>(data, pos));
        case DataType::kInt64:
            return Variant::int64(decode_int_be<int64_t>(data, pos));
        case DataType::kTime:
            return Variant::time(decode_int_be<int64_t>(data, pos));
        case DataType::kUint8:
            return Variant::uint8(decode_uint_be<uint8_t>(data, pos));
        case DataType::kUint16:
            return Variant::uint16(decode_uint_be<uint16_t>(data, pos));
        case DataType::kUint32:
            return Variant::uint32(decode_uint_be<uint32_t>(data, pos));
        case DataType::kUint64:
            return Variant::uint64(decode_uint_be<uint64_t>(data, pos));
        case DataType::kVersion:
            return Variant::version(decode_uint_be<uint64_t>(data, pos));
        case DataType::kFloat:
            return Variant::float32(decode_float_be(data, pos));
        case DataType::kDouble:
            return Variant::float64(decode_double_be(data, pos));
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String: {
            std::string s = decode_bytes_comparable(data, pos);
            return Variant::string(s);
        }
        case DataType::kBinary: {
            std::string s = decode_bytes_comparable(data, pos);
            return Variant::binary(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(s.data()), s.size()));
        }
        default:
            return Variant::none();
    }
}

// Returns the fixed encoded size for a given type, or 0 for variable-length types.
size_t encoded_fixed_size(DataType type) {
    switch (type) {
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
        default:
            return 0; // Variable-length
    }
}

// Check if a Variant's type is compatible with the expected DataType.
bool type_matches(DataType expected, DataType actual) {
    return expected == actual;
}

} // namespace

// --- RowKey implementation ---

RowKey::RowKey(const ExternalSchema& schema, std::vector<Variant> columns)
    : schema_(&schema), columns_(std::move(columns)) {}

absl::StatusOr<RowKey> RowKey::create(const ExternalSchema& schema, std::vector<Variant> columns) {
    if (columns.empty()) {
        return absl::InvalidArgumentError("RowKey must have at least one column");
    }
    if (columns.size() > schema.num_key_columns()) {
        return absl::InvalidArgumentError(
            absl::StrCat("too many key columns: got ", columns.size(), ", schema defines ",
                         schema.num_key_columns()));
    }
    for (size_t i = 0; i < columns.size(); ++i) {
        DataType expected = schema.key_column_type(i);
        DataType actual = columns[i].type();
        if (columns[i].is_none()) {
            if (!schema.key_column_nullable(i)) {
                return absl::InvalidArgumentError(
                    absl::StrCat("key column ", i, " (", data_type_name(expected),
                                 ") does not allow null"));
            }
            continue;
        }
        if (!type_matches(expected, actual)) {
            return absl::InvalidArgumentError(
                absl::StrCat("key column ", i, " type mismatch: expected ",
                             data_type_name(expected), ", got ", data_type_name(actual)));
        }
    }
    return RowKey(schema, std::move(columns));
}

RowKey RowKey::create_unchecked(const ExternalSchema& schema, std::vector<Variant> columns) {
    return RowKey(schema, std::move(columns));
}

// --- Accessors ---

size_t RowKey::num_columns() const {
    return columns_.size();
}

const Variant& RowKey::column(size_t idx) const {
    assert(idx < columns_.size());
    return columns_[idx];
}

const Variant& RowKey::operator[](size_t idx) const {
    return column(idx);
}

bool RowKey::empty() const {
    return columns_.empty();
}

bool RowKey::is_prefix() const {
    return columns_.size() < schema_->num_key_columns();
}

bool RowKey::is_full() const {
    return columns_.size() == schema_->num_key_columns();
}

const ExternalSchema& RowKey::schema() const {
    return *schema_;
}

// --- Comparison ---

std::strong_ordering RowKey::compare(const RowKey& other) const {
    size_t n = std::min(columns_.size(), other.columns_.size());
    for (size_t i = 0; i < n; ++i) {
        auto ord = columns_[i] <=> other.columns_[i];
        if (ord != std::strong_ordering::equal) {
            // Apply descending: invert.
            if (schema_->key_column(i).order == SortOrder::kDescending) {
                if (ord == std::strong_ordering::less)
                    return std::strong_ordering::greater;
                return std::strong_ordering::less;
            }
            return ord;
        }
    }
    // All overlapping columns equal. Fewer columns < more columns.
    return columns_.size() <=> other.columns_.size();
}

bool RowKey::operator==(const RowKey& other) const {
    return compare(other) == std::strong_ordering::equal;
}

bool RowKey::operator<(const RowKey& other) const {
    return compare(other) == std::strong_ordering::less;
}

bool RowKey::operator<=(const RowKey& other) const {
    return compare(other) != std::strong_ordering::greater;
}

bool RowKey::operator>(const RowKey& other) const {
    return compare(other) == std::strong_ordering::greater;
}

bool RowKey::operator>=(const RowKey& other) const {
    return compare(other) != std::strong_ordering::less;
}

bool RowKey::is_prefix_of(const RowKey& other) const {
    if (columns_.size() >= other.columns_.size()) {
        return false;
    }
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (!(columns_[i] == other.columns_[i])) {
            return false;
        }
    }
    return true;
}

// --- Encoding ---

void RowKey::encode_to(std::string& out) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        DataType type = schema_->key_column_type(i);
        size_t start = out.size();
        encode_column(columns_[i], type, out);
        if (schema_->key_column(i).order == SortOrder::kDescending) {
            apply_descending(out, start);
        }
    }
}

std::string RowKey::encode() const {
    std::string result;
    encode_to(result);
    return result;
}

// --- Decoding ---

absl::StatusOr<RowKey> RowKey::decode(const ExternalSchema& schema, std::string_view encoded) {
    std::vector<Variant> columns;
    size_t pos = 0;
    size_t num_keys = schema.num_key_columns();

    for (size_t i = 0; i < num_keys && pos < encoded.size(); ++i) {
        DataType type = schema.key_column_type(i);
        bool desc = (schema.key_column(i).order == SortOrder::kDescending);

        if (desc) {
            // For descending columns, we need to un-invert the bytes first.
            // Determine the extent of this column's encoded bytes.
            size_t col_size = encoded_fixed_size(type);
            if (col_size > 0) {
                // Fixed-size: we know exact byte count.
                if (pos + col_size > encoded.size()) {
                    break; // Partial key.
                }
                std::string inverted(encoded.substr(pos, col_size));
                for (auto& c : inverted) {
                    c = static_cast<char>(~static_cast<unsigned char>(c));
                }
                size_t tmp_pos = 0;
                columns.push_back(decode_column(type, inverted, tmp_pos));
                pos += col_size;
            } else {
                // Variable-length: invert the remaining bytes, decode one column,
                // then advance pos by the consumed amount.
                std::string inverted(encoded.substr(pos));
                for (auto& c : inverted) {
                    c = static_cast<char>(~static_cast<unsigned char>(c));
                }
                size_t tmp_pos = 0;
                columns.push_back(decode_column(type, inverted, tmp_pos));
                pos += tmp_pos;
            }
        } else {
            columns.push_back(decode_column(type, encoded, pos));
        }
    }

    if (columns.empty()) {
        return absl::InvalidArgumentError("failed to decode any key column from encoded data");
    }
    return RowKey(schema, std::move(columns));
}

} // namespace pl::sstv2::types
