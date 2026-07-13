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

#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/varint.h"
#include "cpp/pl/sstv2/types/row.h"

namespace pl::sstv2::block {
namespace {

using types::ColumnFlag;
using types::DataType;
using types::InternalRow;
using types::InternalSchema;
using types::OpType;
using types::Schema;
using types::SchemaBuilder;
using types::Value;
using types::Version;

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder()
                      .add_column("user", DataType::kString)
                      .add_column("bucket", DataType::kUint32)
                      .build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

void write_fixed64(std::string* bytes, size_t offset, uint64_t value) {
    uint8_t encoded[8];
    codec::encode_fixed64(encoded, value);
    bytes->replace(
        offset, sizeof(encoded), reinterpret_cast<const char*>(encoded), sizeof(encoded));
}

void refresh_block_checksum(std::string* block) {
    write_fixed64(block, 44, 0);
    write_fixed64(block, 44, codec::crc32c_u64(*block));
}

std::vector<uint64_t> read_column_offsets(std::string_view block,
                                          const InternalSchema::ConstRef& schema,
                                          std::vector<size_t>* positions = nullptr,
                                          std::vector<size_t>* lengths = nullptr) {
    const size_t table = static_cast<size_t>(codec::read_fixed64(block, 20));
    size_t pos = table;
    std::vector<uint64_t> offsets;
    for (size_t i = 0; i < schema->column_count(); ++i) {
        uint64_t value = 0;
        const size_t n = codec::decode_varint(
            reinterpret_cast<const uint8_t*>(block.data() + pos), block.size() - pos, &value);
        EXPECT_NE(n, 0u);
        if (positions != nullptr)
            positions->push_back(pos);
        if (lengths != nullptr)
            lengths->push_back(n);
        offsets.push_back(value);
        pos += n;
    }
    return offsets;
}

void write_varint_with_size(std::string* bytes, size_t pos, size_t size, uint64_t value) {
    std::string encoded;
    codec::encode_varint(value, &encoded);
    ASSERT_LE(encoded.size(), size);
    while (encoded.size() < size) {
        encoded.back() = static_cast<char>(static_cast<uint8_t>(encoded.back()) | 0x80);
        encoded.push_back(0);
    }
    bytes->replace(pos, size, encoded);
}

InternalRow make_row(InternalSchema::ConstRef schema,
                     std::string user,
                     uint32_t bucket,
                     Version version,
                     std::string_view value) {
    auto row = InternalRow::make(schema);
    row.columns[0] = Value::make<DataType::kString>(std::move(user));
    row.columns[1] = Value::make<DataType::kUint32>(bucket);
    row.columns[schema->version_index()] = Value::make<DataType::kVersion>(version);
    row.columns[schema->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema->flag_index()] =
        Value::make<DataType::kUint64>(ColumnFlag::for_value(DataType::kString, true).raw());
    row.columns[schema->filename_index()] = Value::make<DataType::kString>("@1");
    row.columns[schema->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->length_index()] = Value::make<DataType::kUint64>(value.size());
    row.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{123});
    return row;
}

TEST(BlockTest, DataBlockRoundTripNoCompression) {
    auto schema_ptr = InternalSchema::make(make_schema());
    BlockBuilder builder(schema_ptr, Options{.kind = Kind::kData});

    ASSERT_TRUE(
        builder.add(make_row(schema_ptr, "alice", 7, Version{.major = 10, .minor = 1}, "v1")).ok());
    ASSERT_TRUE(
        builder.add(make_row(schema_ptr, "bob", 9, Version{.major = 8, .minor = 0}, "v2")).ok());

    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    auto reader = BlockReader::open(*encoded, schema_ptr, Kind::kData);
    ASSERT_TRUE(reader.ok()) << reader.status();
    ASSERT_EQ(reader->rows().size(), 2u);
    EXPECT_EQ(reader->rows()[0].columns[0].as_string(), "alice");
    EXPECT_EQ(reader->rows()[0].columns[1].as_uint32(), 7u);
    EXPECT_EQ(reader->rows()[0].version(schema_ptr).major, 10u);
    EXPECT_EQ(reader->rows()[0].version(schema_ptr).minor, 1u);
    EXPECT_EQ(reader->rows()[1].columns[0].as_string(), "bob");
    EXPECT_EQ(reader->header().magic, Kind::kData);
    EXPECT_EQ(reader->header().row_count, 2u);
}

TEST(BlockTest, DataBlockRoundTripSnappy) {
    auto schema_ptr = InternalSchema::make(make_schema());
    Options options;
    options.kind = Kind::kData;
    options.compression.codec = compress::Codec::kSnappy;
    BlockBuilder builder(schema_ptr, options);

    ASSERT_TRUE(
        builder.add(make_row(schema_ptr, "carol", 3, Version{.major = 2, .minor = 5}, "v")).ok());

    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    auto reader = BlockReader::open(*encoded, schema_ptr, Kind::kData);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->rows()[0].columns[0].as_string(), "carol");
    EXPECT_EQ(decode_block_flag(reader->header().flags), compress::Codec::kSnappy);
}

TEST(BlockTest, ArrayAndMapColumnsRoundTrip) {
    auto user_schema = SchemaBuilder()
                           .add_column("path", DataType::kArray)
                           .add_column("attrs", DataType::kMap)
                           .build();
    ASSERT_TRUE(user_schema.has_value());
    auto schema_ptr = InternalSchema::make(std::make_shared<const Schema>(std::move(*user_schema)));
    BlockBuilder builder(schema_ptr, Options{.kind = Kind::kData});

    InternalRow row = InternalRow::make(schema_ptr);
    row.columns[0] = Value::make_array({
        Value::make<DataType::kString>("tenant"),
        Value::make<DataType::kUint64>(uint64_t{7}),
    });
    row.columns[1] = Value::make_map({
        {Value::make<DataType::kString>("b"), Value::make<DataType::kUint64>(uint64_t{2})},
        {Value::make<DataType::kString>("a"), Value::make<DataType::kUint64>(uint64_t{1})},
    });
    row.columns[schema_ptr->version_index()] = Value::make<DataType::kVersion>(Version{.major = 1});
    row.columns[schema_ptr->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema_ptr->flag_index()] =
        Value::make<DataType::kUint64>(ColumnFlag::for_value(DataType::kString, true).raw());
    row.columns[schema_ptr->filename_index()] = Value::make<DataType::kString>("@1");
    row.columns[schema_ptr->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema_ptr->length_index()] = Value::make<DataType::kUint64>(uint64_t{1});
    row.columns[schema_ptr->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});

    ASSERT_TRUE(builder.add(std::move(row), "x").ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    auto reader = BlockReader::open(*encoded, schema_ptr, Kind::kData);
    ASSERT_TRUE(reader.ok()) << reader.status();
    ASSERT_EQ(reader->rows().size(), 1u);
    ASSERT_EQ(reader->rows()[0].columns[0].as_array().size(), 2u);
    EXPECT_EQ(reader->rows()[0].columns[0].as_array()[0].as_string(), "tenant");
    EXPECT_EQ(reader->rows()[0].columns[0].as_array()[1].as_uint64(), 7u);
    ASSERT_EQ(reader->rows()[0].columns[1].as_map().size(), 2u);
    EXPECT_EQ(reader->rows()[0].columns[1].as_map()[0].first.as_string(), "a");
    EXPECT_EQ(reader->rows()[0].columns[1].as_map()[1].first.as_string(), "b");
}

TEST(BlockTest, RejectsColumnRowCountMismatch) {
    auto schema = InternalSchema::make(make_schema());
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(make_row(schema, "alice", 7, Version{.major = 1}, "v")).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    write_fixed64(&*encoded, 12, 2);
    refresh_block_checksum(&*encoded);
    EXPECT_FALSE(BlockReader::open(*encoded, schema, Kind::kData).ok());
}

TEST(BlockTest, RejectsStringReferenceOverflow) {
    auto schema = InternalSchema::make(make_schema());
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(make_row(schema, "alice", 7, Version{.major = 1}, "v")).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    const auto offsets = read_column_offsets(*encoded, schema);
    size_t pos = static_cast<size_t>(offsets[0]);
    ASSERT_EQ(static_cast<uint8_t>((*encoded)[pos++]), 100u);
    uint64_t sub_count = 0;
    pos += codec::decode_varint(
        reinterpret_cast<const uint8_t*>(encoded->data() + pos), encoded->size() - pos, &sub_count);
    ASSERT_EQ(sub_count, 2u);
    ASSERT_EQ(static_cast<uint8_t>((*encoded)[pos++]), static_cast<uint8_t>(DataType::kUint64));
    uint64_t raw_offset = 0;
    pos += codec::decode_varint(reinterpret_cast<const uint8_t*>(encoded->data() + pos),
                                encoded->size() - pos,
                                &raw_offset);
    ASSERT_EQ(raw_offset, 0u);
    ASSERT_EQ(static_cast<uint8_t>((*encoded)[pos++]), static_cast<uint8_t>(DataType::kUint64));
    uint64_t length_unit_offset = 0;
    pos += codec::decode_varint(reinterpret_cast<const uint8_t*>(encoded->data() + pos),
                                encoded->size() - pos,
                                &length_unit_offset);
    const size_t offset_cell = pos + 2;
    write_fixed64(&*encoded, offset_cell, std::numeric_limits<uint64_t>::max());
    refresh_block_checksum(&*encoded);
    EXPECT_FALSE(BlockReader::open(*encoded, schema, Kind::kData).ok());
}

TEST(BlockTest, RejectsInvalidAndNonMonotonicColumnOffsets) {
    auto schema = InternalSchema::make(make_schema());
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(make_row(schema, "alice", 7, Version{.major = 1}, "v")).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    std::vector<size_t> positions;
    std::vector<size_t> lengths;
    const auto offsets = read_column_offsets(*encoded, schema, &positions, &lengths);
    std::string below_header = *encoded;
    write_varint_with_size(&below_header, positions[0], lengths[0], Header::kSize - 1);
    refresh_block_checksum(&below_header);
    EXPECT_FALSE(BlockReader::open(below_header, schema, Kind::kData).ok());

    std::string non_monotonic = *encoded;
    write_varint_with_size(&non_monotonic, positions[1], lengths[1], offsets[0] - 1);
    refresh_block_checksum(&non_monotonic);
    EXPECT_FALSE(BlockReader::open(non_monotonic, schema, Kind::kData).ok());
}

TEST(BlockTest, RejectsInvalidColumnFlag) {
    auto schema = InternalSchema::make(make_schema());
    auto row = make_row(schema, "alice", 7, Version{.major = 1}, "v");
    row.columns[schema->flag_index()] = Value::make<DataType::kUint64>(1ULL << 10);
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(std::move(row)).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_FALSE(BlockReader::open(*encoded, schema, Kind::kData).ok());
}

TEST(BlockTest, RejectsExcessiveHeaderRowCountBeforeAllocating) {
    auto schema = InternalSchema::make(make_schema());
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(make_row(schema, "alice", 7, Version{.major = 1}, "v")).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    write_fixed64(&*encoded, 12, std::numeric_limits<uint64_t>::max());
    refresh_block_checksum(&*encoded);
    EXPECT_FALSE(BlockReader::open(*encoded, schema, Kind::kData).ok());
}

TEST(BlockTest, RejectsUnknownFlagsAndCodec) {
    auto schema = InternalSchema::make(make_schema());
    BlockBuilder builder(schema, Options{.kind = Kind::kData});
    ASSERT_TRUE(builder.add(make_row(schema, "alice", 7, Version{.major = 1}, "v")).ok());
    auto encoded = builder.finish();
    ASSERT_TRUE(encoded.ok()) << encoded.status();

    std::string reserved = *encoded;
    write_fixed64(&reserved, 4, codec::read_fixed64(reserved, 4) | (1ULL << 10));
    refresh_block_checksum(&reserved);
    EXPECT_FALSE(BlockReader::open(reserved, schema, Kind::kData).ok());

    std::string unknown_codec = *encoded;
    write_fixed64(&unknown_codec, 4, encode_block_flag(static_cast<compress::Codec>(255)));
    refresh_block_checksum(&unknown_codec);
    EXPECT_FALSE(BlockReader::open(unknown_codec, schema, Kind::kData).ok());
}

TEST(BlockTest, EnforcesConfiguredRowLimit) {
    auto schema_ptr = InternalSchema::make(make_schema());

    Options row_options;
    row_options.kind = Kind::kData;
    row_options.max_row_count = 1;
    BlockBuilder row_limited(schema_ptr, row_options);
    ASSERT_TRUE(
        row_limited.add(make_row(schema_ptr, "alice", 7, Version{.major = 10, .minor = 1}, "v1"))
            .ok());
    EXPECT_FALSE(
        row_limited.add(make_row(schema_ptr, "bob", 9, Version{.major = 8, .minor = 0}, "v2"))
            .ok());

    // Single-row blocks are allowed to exceed the hard size limit (PDF §6.1).
    Options size_options;
    size_options.kind = Kind::kData;
    size_options.max_block_size_hard_limit = Header::kSize;
    BlockBuilder size_limited(schema_ptr, size_options);
    ASSERT_TRUE(
        size_limited.add(make_row(schema_ptr, "carol", 3, Version{.major = 2, .minor = 5}, "v"))
            .ok());
    EXPECT_TRUE(size_limited.finish().ok());
}

} // namespace
} // namespace pl::sstv2::block
