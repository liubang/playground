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

#include "cpp/pl/sstv2/block/block.h"

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/checksum.h"
#include "cpp/pl/sstv2/codec/endian.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/varint.h"
#include "cpp/pl/sstv2/types/value_codec.h"
#include "cpp/pl/sstv2/pattern/compound.h"
#include "cpp/pl/sstv2/pattern/raw.h"

namespace pl::sstv2::block {

#define SSTV2_RETURN_IF_ERROR(expr) \
    do {                            \
        auto _status = (expr);      \
        if (!_status.ok())          \
            return _status;         \
    } while (false)

namespace {

using types::DataType;
using types::Value;

void encode_header(const Header& h, std::string* dst) {
    codec::append_fixed32(dst, static_cast<uint32_t>(h.magic));
    codec::append_fixed64(dst, h.flags);
    codec::append_fixed64(dst, h.row_count);
    codec::append_fixed64(dst, h.offset_table_offset);
    codec::append_fixed64(dst, h.uncompressed_block_length);
    codec::append_fixed64(dst, h.compressed_block_length);
    codec::append_fixed64(dst, h.checksum);
}

Header decode_header(std::string_view input) {
    Header h;
    h.magic = static_cast<Kind>(codec::read_fixed32(input, 0));
    h.flags = codec::read_fixed64(input, 4);
    h.row_count = codec::read_fixed64(input, 12);
    h.offset_table_offset = codec::read_fixed64(input, 20);
    h.uncompressed_block_length = codec::read_fixed64(input, 28);
    h.compressed_block_length = codec::read_fixed64(input, 36);
    h.checksum = codec::read_fixed64(input, 44);
    return h;
}

absl::Status verify_value_type(const Value& value, DataType type, std::string_view name) {
    if (value.type() != type) {
        return absl::InvalidArgumentError(absl::StrCat("column ",
                                                       name,
                                                       " expects ",
                                                       types::data_type_name(type),
                                                       ", got ",
                                                       types::data_type_name(value.type())));
    }
    return absl::OkStatus();
}

template <size_t CellSize, typename T>
void add_raw_cell(pattern::RawEncoder<CellSize>* encoder, T value) {
    uint8_t buf[CellSize];
    if constexpr (CellSize == 1) {
        static_assert(sizeof(value) == 1);
        std::memcpy(buf, &value, sizeof(value));
    } else if constexpr (CellSize == 2) {
        codec::encode_fixed16(buf, static_cast<uint16_t>(value));
    } else if constexpr (CellSize == 4) {
        if constexpr (std::is_same_v<T, float>) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            codec::encode_fixed32(buf, bits);
        } else {
            codec::encode_fixed32(buf, static_cast<uint32_t>(value));
        }
    } else if constexpr (CellSize == 8) {
        if constexpr (std::is_same_v<T, double>) {
            uint64_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            codec::encode_fixed64(buf, bits);
        } else {
            codec::encode_fixed64(buf, static_cast<uint64_t>(value));
        }
    }
    encoder->add(buf);
}

absl::StatusOr<std::string> encode_column(const std::vector<types::InternalRow>& rows,
                                          const types::InternalSchema::ConstRef& schema,
                                          size_t column,
                                          std::string* data_table) {
    const DataType type = schema->column_type(column);
    switch (type) {
        case DataType::kBool: {
            pattern::RawEncoder<1> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                enc.add(static_cast<uint8_t>(row.columns[column].as_bool() ? 1 : 0));
            }
            return enc.finish().data;
        }
        case DataType::kInt8:
        case DataType::kUint8: {
            pattern::RawEncoder<1> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                enc.add(type == DataType::kInt8
                            ? static_cast<uint8_t>(row.columns[column].as_int8())
                            : row.columns[column].as_uint8());
            }
            return enc.finish().data;
        }
        case DataType::kInt16:
        case DataType::kUint16: {
            pattern::RawEncoder<2> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                add_raw_cell(&enc,
                             type == DataType::kInt16
                                 ? static_cast<uint16_t>(row.columns[column].as_int16())
                                 : row.columns[column].as_uint16());
            }
            return enc.finish().data;
        }
        case DataType::kInt32:
        case DataType::kUint32:
        case DataType::kFloat: {
            pattern::RawEncoder<4> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                if (type == DataType::kFloat) {
                    add_raw_cell(&enc, row.columns[column].as_float());
                } else {
                    add_raw_cell(&enc,
                                 type == DataType::kInt32
                                     ? static_cast<uint32_t>(row.columns[column].as_int32())
                                     : row.columns[column].as_uint32());
                }
            }
            return enc.finish().data;
        }
        case DataType::kInt64:
        case DataType::kUint64:
        case DataType::kDouble: {
            pattern::RawEncoder<8> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                if (type == DataType::kDouble) {
                    add_raw_cell(&enc, row.columns[column].as_double());
                } else {
                    add_raw_cell(&enc,
                                 type == DataType::kInt64
                                     ? static_cast<uint64_t>(row.columns[column].as_int64())
                                     : row.columns[column].as_uint64());
                }
            }
            return enc.finish().data;
        }
        case DataType::kLongDouble: {
            pattern::RawEncoder<16> enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                enc.add(row.columns[column].as_long_double().data);
            }
            return enc.finish().data;
        }
        case DataType::kTime: {
            pattern::TimeEncoder enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                const auto& t = row.columns[column].as_time();
                enc.add(t.seconds, t.nanoseconds);
            }
            return enc.finish().data;
        }
        case DataType::kVersion: {
            pattern::VersionEncoder enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                const auto& v = row.columns[column].as_version();
                enc.add(v.major, v.minor);
            }
            return enc.finish().data;
        }
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary: {
            pattern::StringRefEncoder enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                const std::string_view bytes = row.columns[column].as_string();
                const uint64_t offset = data_table->size();
                data_table->append(bytes);
                enc.add(offset, bytes.size());
            }
            return enc.finish().data;
        }
        case DataType::kArray:
        case DataType::kMap: {
            pattern::StringRefEncoder enc;
            enc.reserve(rows.size());
            for (const auto& row : rows) {
                SSTV2_RETURN_IF_ERROR(
                    verify_value_type(row.columns[column], type, schema->column_name(column)));
                auto bytes = types::encode_value(row.columns[column]);
                if (!bytes.ok())
                    return bytes.status();
                const uint64_t offset = data_table->size();
                data_table->append(*bytes);
                enc.add(offset, bytes->size());
            }
            return enc.finish().data;
        }
        default:
            return absl::UnimplementedError(absl::StrCat(
                "block column type ", types::data_type_name(type), " is not implemented yet"));
    }
}

template <typename T> Value make_inline(DataType type, T value) {
    switch (type) {
        case DataType::kInt8:
            return Value::make<DataType::kInt8>(static_cast<int8_t>(value));
        case DataType::kUint8:
            return Value::make<DataType::kUint8>(static_cast<uint8_t>(value));
        case DataType::kInt16:
            return Value::make<DataType::kInt16>(static_cast<int16_t>(value));
        case DataType::kUint16:
            return Value::make<DataType::kUint16>(static_cast<uint16_t>(value));
        case DataType::kInt32:
            return Value::make<DataType::kInt32>(static_cast<int32_t>(value));
        case DataType::kUint32:
            return Value::make<DataType::kUint32>(static_cast<uint32_t>(value));
        case DataType::kInt64:
            return Value::make<DataType::kInt64>(static_cast<int64_t>(value));
        case DataType::kUint64:
            return Value::make<DataType::kUint64>(static_cast<uint64_t>(value));
        default:
            return {};
    }
}

absl::Status decode_column(std::string_view unit,
                           std::string_view data_table,
                           const types::InternalSchema::ConstRef& schema,
                           size_t column,
                           std::vector<types::InternalRow>* rows) {
    const DataType type = schema->column_type(column);
    switch (type) {
        case DataType::kBool: {
            pattern::RawDecoder<1> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad bool raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = Value::make<DataType::kBool>(dec.get(i) != 0);
            }
            return absl::OkStatus();
        }
        case DataType::kInt8:
        case DataType::kUint8: {
            pattern::RawDecoder<1> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad 1-byte raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = make_inline(type, dec.get(i));
            }
            return absl::OkStatus();
        }
        case DataType::kInt16:
        case DataType::kUint16: {
            pattern::RawDecoder<2> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad 2-byte raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = make_inline(type, dec.get(i));
            }
            return absl::OkStatus();
        }
        case DataType::kInt32:
        case DataType::kUint32: {
            pattern::RawDecoder<4> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad 4-byte raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = make_inline(type, dec.get(i));
            }
            return absl::OkStatus();
        }
        case DataType::kFloat: {
            pattern::RawDecoder<4> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad float raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                uint32_t bits = dec.get(i);
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                (*rows)[i].columns[column] = Value::make<DataType::kFloat>(f);
            }
            return absl::OkStatus();
        }
        case DataType::kInt64:
        case DataType::kUint64: {
            pattern::RawDecoder<8> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad 8-byte raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = make_inline(type, dec.get(i));
            }
            return absl::OkStatus();
        }
        case DataType::kDouble: {
            pattern::RawDecoder<8> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad double raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                uint64_t bits = dec.get(i);
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                (*rows)[i].columns[column] = Value::make<DataType::kDouble>(d);
            }
            return absl::OkStatus();
        }
        case DataType::kLongDouble: {
            pattern::RawDecoder<16> dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad long double raw unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                types::LongDouble value;
                dec.get(i, value.data);
                (*rows)[i].columns[column] = Value::make<DataType::kLongDouble>(value);
            }
            return absl::OkStatus();
        }
        case DataType::kTime: {
            pattern::TimeDecoder dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad time unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = Value::make<DataType::kTime>(
                    types::Time{.seconds = dec.seconds(i), .nanoseconds = dec.nanoseconds(i)});
            }
            return absl::OkStatus();
        }
        case DataType::kVersion: {
            pattern::VersionDecoder dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad version unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                (*rows)[i].columns[column] = Value::make<DataType::kVersion>(
                    types::Version{.major = dec.major(i), .minor = dec.minor(i)});
            }
            return absl::OkStatus();
        }
        case DataType::kString:
        case DataType::kU16String:
        case DataType::kU32String:
        case DataType::kBinary: {
            pattern::StringRefDecoder dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad string-ref unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                const uint64_t off = dec.offset(i);
                const uint64_t len = dec.length(i);
                if (off + len > data_table.size()) {
                    return absl::InvalidArgumentError("string-ref points outside data table");
                }
                std::string bytes(data_table.data() + off, static_cast<size_t>(len));
                switch (type) {
                    case DataType::kString:
                        (*rows)[i].columns[column] =
                            Value::make<DataType::kString>(std::move(bytes));
                        break;
                    case DataType::kU16String:
                        (*rows)[i].columns[column] =
                            Value::make<DataType::kU16String>(std::move(bytes));
                        break;
                    case DataType::kU32String:
                        (*rows)[i].columns[column] =
                            Value::make<DataType::kU32String>(std::move(bytes));
                        break;
                    default:
                        (*rows)[i].columns[column] =
                            Value::make<DataType::kBinary>(std::move(bytes));
                        break;
                }
            }
            return absl::OkStatus();
        }
        case DataType::kArray:
        case DataType::kMap: {
            pattern::StringRefDecoder dec;
            if (!dec.parse(unit))
                return absl::InvalidArgumentError("bad variant-ref unit");
            for (size_t i = 0; i < rows->size(); ++i) {
                const uint64_t off = dec.offset(i);
                const uint64_t len = dec.length(i);
                if (off + len > data_table.size()) {
                    return absl::InvalidArgumentError("variant-ref points outside data table");
                }
                auto value = types::decode_value(
                    type, std::string_view(data_table.data() + off, static_cast<size_t>(len)));
                if (!value.ok())
                    return value.status();
                (*rows)[i].columns[column] = std::move(*value);
            }
            return absl::OkStatus();
        }
        default:
            return absl::UnimplementedError(absl::StrCat(
                "block column type ", types::data_type_name(type), " is not implemented yet"));
    }
}

} // namespace

BlockBuilder::BlockBuilder(types::InternalSchema::ConstRef schema, Options options)
    : schema_(std::move(schema)), options_(options) {}

absl::Status BlockBuilder::add(types::InternalRow row) {
    return add(std::move(row), std::string{});
}

absl::Status BlockBuilder::add(types::InternalRow row, std::string embedded_value) {
    if (schema_ == nullptr) {
        return absl::InvalidArgumentError("block builder schema is null");
    }
    if (rows_.size() >= options_.max_row_count) {
        return absl::ResourceExhaustedError("block row count limit exceeded");
    }
    if (row.columns.size() != schema_->column_count()) {
        return absl::InvalidArgumentError("internal row column count mismatch");
    }
    rows_.push_back(std::move(row));
    embedded_values_.push_back(std::move(embedded_value));
    return absl::OkStatus();
}

absl::StatusOr<std::string> BlockBuilder::finish() const {
    std::string data_table;
    std::vector<std::string> units;
    units.reserve(schema_->column_count());
    std::vector<types::InternalRow> rows = rows_;
    for (size_t column = 0; column < schema_->column_count(); ++column) {
        if (column == schema_->offset_index()) {
            for (size_t row = 0; row < rows.size(); ++row) {
                if (!embedded_values_[row].empty()) {
                    rows[row].columns[schema_->offset_index()] =
                        Value::make<DataType::kUint64>(Header::kSize + data_table.size());
                    data_table.append(embedded_values_[row]);
                }
            }
        }
        auto unit = encode_column(rows, schema_, column, &data_table);
        if (!unit.ok())
            return unit.status();
        units.push_back(std::move(*unit));
    }

    std::string body;
    body.append(data_table);
    std::vector<uint64_t> offsets;
    offsets.reserve(units.size());
    for (const auto& unit : units) {
        offsets.push_back(Header::kSize + body.size());
        body.append(unit);
    }

    const uint64_t offset_table_offset = Header::kSize + body.size();
    for (uint64_t off : offsets) {
        codec::encode_varint(off, &body);
    }

    auto payload = compress::compress(body, options_.compression);
    if (!payload.ok())
        return payload.status();

    Header h;
    h.magic = options_.kind;
    h.flags = compress::encode_block_flag(options_.compression.codec);
    h.row_count = rows.size();
    h.offset_table_offset = offset_table_offset;
    h.uncompressed_block_length = Header::kSize + body.size();
    h.compressed_block_length =
        options_.compression.codec == compress::Codec::kNone ? 0 : Header::kSize + payload->size();

    std::string header_zero;
    encode_header(h, &header_zero);
    std::string block = header_zero;
    block.append(*payload);
    h.checksum = codec::crc32c_u64(block);

    block.clear();
    encode_header(h, &block);
    block.append(*payload);
    return block;
}

absl::StatusOr<BlockReader> BlockReader::open(std::string_view block,
                                              const types::InternalSchema::ConstRef& schema,
                                              Kind expected) {
    if (block.size() < Header::kSize) {
        return absl::InvalidArgumentError("block is shorter than header");
    }
    Header h = decode_header(block.substr(0, Header::kSize));
    if (h.magic != expected) {
        return absl::InvalidArgumentError("block magic mismatch");
    }
    const auto codec = compress::decode_block_flag(h.flags);
    const uint64_t expected_block_length =
        codec == compress::Codec::kNone ? h.uncompressed_block_length : h.compressed_block_length;
    if (expected_block_length != block.size()) {
        return absl::InvalidArgumentError("block length mismatch");
    }

    Header zero = h;
    zero.checksum = 0;
    std::string checksum_input;
    encode_header(zero, &checksum_input);
    checksum_input.append(block.substr(Header::kSize));
    if (codec::crc32c_u64(checksum_input) != h.checksum) {
        return absl::InvalidArgumentError("block checksum mismatch");
    }

    auto body = compress::uncompress(
        block.substr(Header::kSize), codec, h.uncompressed_block_length - Header::kSize);
    if (!body.ok())
        return body.status();

    if (h.offset_table_offset < Header::kSize ||
        h.offset_table_offset > h.uncompressed_block_length) {
        return absl::InvalidArgumentError("bad offset table offset");
    }
    const auto offset_table_body = static_cast<size_t>(h.offset_table_offset - Header::kSize);
    if (offset_table_body > body->size()) {
        return absl::InvalidArgumentError("offset table outside block body");
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(schema->column_count());
    size_t pos = offset_table_body;
    for (size_t i = 0; i < schema->column_count(); ++i) {
        uint64_t off = 0;
        const size_t n = codec::decode_varint(
            reinterpret_cast<const uint8_t*>(body->data() + pos), body->size() - pos, &off);
        if (n == 0)
            return absl::InvalidArgumentError("bad column offset table");
        offsets.push_back(off);
        pos += n;
    }
    if (offsets.empty())
        return absl::InvalidArgumentError("empty column offset table");
    const auto data_table_len = static_cast<size_t>(offsets.front() - Header::kSize);
    if (data_table_len > offset_table_body) {
        return absl::InvalidArgumentError("bad data table length");
    }
    const std::string_view data_table(body->data(), data_table_len);

    BlockReader reader;
    reader.header_ = h;
    reader.data_table_.assign(data_table);
    reader.rows_.reserve(static_cast<size_t>(h.row_count));
    for (uint64_t i = 0; i < h.row_count; ++i) {
        reader.rows_.push_back(types::InternalRow::make(schema));
    }

    for (size_t column = 0; column < schema->column_count(); ++column) {
        const auto begin = static_cast<size_t>(offsets[column] - Header::kSize);
        const size_t end = column + 1 == schema->column_count()
                               ? offset_table_body
                               : static_cast<size_t>(offsets[column + 1] - Header::kSize);
        if (begin > end || end > offset_table_body) {
            return absl::InvalidArgumentError("bad column unit range");
        }
        auto status = decode_column(std::string_view(body->data() + begin, end - begin),
                                    data_table,
                                    schema,
                                    column,
                                    &reader.rows_);
        if (!status.ok())
            return status;
    }
    return reader;
}

absl::StatusOr<std::string_view> BlockReader::embedded_value(
    size_t row_index, const types::InternalSchema::ConstRef& schema) const {
    if (row_index >= rows_.size()) {
        return absl::InvalidArgumentError("embedded value row index out of range");
    }
    const auto& row = rows_[row_index];
    const uint64_t offset = row.offset(schema);
    const uint64_t length = row.length(schema);
    if (offset < Header::kSize) {
        return absl::InvalidArgumentError("embedded value offset is before block body");
    }
    const uint64_t data_table_offset = offset - Header::kSize;
    if (data_table_offset > data_table_.size() || length > data_table_.size() - data_table_offset) {
        return absl::InvalidArgumentError("embedded value points outside data table");
    }
    return std::string_view(data_table_.data() + data_table_offset, static_cast<size_t>(length));
}

} // namespace pl::sstv2::block

#undef SSTV2_RETURN_IF_ERROR
