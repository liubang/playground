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
#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/types/op_type.h"

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
    EXPECT_EQ(compress::decode_block_flag(reader->header().flags), compress::Codec::kSnappy);
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
