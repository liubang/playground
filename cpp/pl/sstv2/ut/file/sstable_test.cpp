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

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/format/tail.h"
#include "cpp/pl/sstv2/types/op_type.h"

namespace pl::sstv2::file {
namespace {

using types::DataType;
using types::OpType;
using types::Row;
using types::Schema;
using types::SchemaBuilder;
using types::Value;
using types::Version;

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder()
                      .add_column("tenant", DataType::kString)
                      .add_column("id", DataType::kUint64)
                      .build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

Row make_row(std::string tenant, uint64_t id, Version version, std::string value) {
    Row row;
    row.key_columns.push_back(Value::make<DataType::kString>(std::move(tenant)));
    row.key_columns.push_back(Value::make<DataType::kUint64>(id));
    row.version = version;
    row.op_type = OpType::kPut;
    row.value = Value::make<DataType::kString>(std::move(value));
    return row;
}

Row make_bool_row(std::string tenant, uint64_t id, Version version, bool value) {
    Row row;
    row.key_columns.push_back(Value::make<DataType::kString>(std::move(tenant)));
    row.key_columns.push_back(Value::make<DataType::kUint64>(id));
    row.version = version;
    row.op_type = OpType::kPut;
    row.value = Value::make<DataType::kBool>(value);
    return row;
}

Row make_complex_row(uint64_t id, std::string value) {
    Row row;
    row.key_columns.push_back(Value::make_array({
        Value::make<DataType::kString>("tenant"),
        Value::make<DataType::kUint64>(id),
    }));
    row.key_columns.push_back(Value::make_map({
        {Value::make<DataType::kString>("id"), Value::make<DataType::kUint64>(id)},
        {Value::make<DataType::kString>("kind"), Value::make<DataType::kString>("event")},
    }));
    row.version = Version{.major = 10, .minor = id};
    row.op_type = OpType::kPut;
    row.value = Value::make<DataType::kString>(std::move(value));
    return row;
}

uint64_t root_index_offset(std::string_view key_file) {
    auto tail = format::decode_tail(key_file.substr(key_file.size() - format::Tail::kSize));
    EXPECT_TRUE(tail.ok()) << tail.status();
    auto locator =
        format::decode_section(key_file.substr(static_cast<size_t>(tail->locator_offset),
                                               static_cast<size_t>(tail->locator_length)),
                               format::SectionMagic::kLocator);
    EXPECT_TRUE(locator.ok()) << locator.status();
    const Value* value = format::find_section_value(locator->entries, "RootIndex_Offset");
    EXPECT_NE(value, nullptr);
    EXPECT_EQ(value->type(), DataType::kUint64);
    return value->as_uint64();
}

std::vector<block::Kind> block_kinds_before_root(std::string_view key_file, uint64_t root_offset) {
    std::vector<block::Kind> kinds;
    size_t offset = 0;
    while (offset < root_offset) {
        const auto kind = static_cast<block::Kind>(codec::read_fixed32(key_file, offset));
        const uint64_t uncompressed_length = codec::read_fixed64(key_file, offset + 28);
        const uint64_t compressed_length = codec::read_fixed64(key_file, offset + 36);
        const uint64_t block_length =
            compressed_length == 0 ? uncompressed_length : compressed_length;
        kinds.push_back(kind);
        offset += static_cast<size_t>(block_length);
    }
    EXPECT_EQ(offset, root_offset);
    return kinds;
}

TEST(SSTableTest, BuildOpenAndScanSeparatedValues) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 1;
    Builder builder(make_schema(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 2, Version{.major = 9, .minor = 0}, "second")).ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    ASSERT_FALSE(files->key_file.empty());
    ASSERT_EQ(files->value_file, "firstsecond");

    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->schema()->row_key_column_count(), 2u);
    EXPECT_EQ(reader->statistics().total_row_count, 2u);
    EXPECT_EQ(reader->statistics().data_block_count, 2u);
    EXPECT_EQ(reader->statistics().index_block_count, 1u);
    EXPECT_EQ(reader->statistics().key_file_size, files->key_file.size());
    EXPECT_EQ(reader->statistics().value_file_size, files->value_file.size());

    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 2u);
    EXPECT_EQ((*rows)[0].key_columns[0].as_string(), "a");
    EXPECT_EQ((*rows)[0].key_columns[1].as_uint64(), 1u);
    EXPECT_EQ((*rows)[0].version.major, 10u);
    EXPECT_EQ((*rows)[0].value.as_string(), "first");
    EXPECT_EQ((*rows)[1].key_columns[0].as_string(), "b");
    EXPECT_EQ((*rows)[1].value.as_string(), "second");

    auto found =
        reader->get({Value::make<DataType::kString>("b"), Value::make<DataType::kUint64>(2)},
                    Version{.major = 9, .minor = 0});
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->value.as_string(), "second");

    auto missing =
        reader->get({Value::make<DataType::kString>("z"), Value::make<DataType::kUint64>(9)},
                    Version{.major = 1, .minor = 0});
    ASSERT_TRUE(missing.ok()) << missing.status();
    EXPECT_FALSE(missing->has_value());
}

TEST(SSTableTest, BuildOpenAndScanEmbeddedValues) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 64;
    Builder builder(make_schema(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "tiny")).ok());
    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    EXPECT_TRUE(files->value_file.empty());

    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].value.as_string(), "tiny");
}

TEST(SSTableTest, BoolValuesAreStoredInColumnFlagOnly) {
    Builder builder(make_schema());

    ASSERT_TRUE(builder.add(make_bool_row("a", 1, Version{.major = 10, .minor = 0}, true)).ok());
    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    EXPECT_TRUE(files->value_file.empty());

    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].value.type(), DataType::kBool);
    EXPECT_TRUE((*rows)[0].value.as_bool());
}

TEST(SSTableTest, RejectsOutOfOrderRows) {
    Builder builder(make_schema());
    ASSERT_TRUE(builder.add(make_row("b", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    EXPECT_FALSE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "second")).ok());
}

TEST(SSTableTest, ScanRejectsValueChecksumMismatch) {
    Builder builder(make_schema());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    files->value_file[0] ^= 0x01;

    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_FALSE(reader->scan().ok());
}

TEST(SSTableTest, ArrayAndMapKeysRoundTrip) {
    auto schema = SchemaBuilder()
                      .add_column("path", DataType::kArray)
                      .add_column("attrs", DataType::kMap)
                      .build();
    ASSERT_TRUE(schema.has_value());
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    Builder builder(std::make_shared<const Schema>(std::move(*schema)), options);

    ASSERT_TRUE(builder.add(make_complex_row(1, "one")).ok());
    ASSERT_TRUE(builder.add(make_complex_row(2, "two")).ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 2u);
    EXPECT_EQ((*rows)[0].key_columns[0].as_array()[1].as_uint64(), 1u);
    EXPECT_EQ((*rows)[1].key_columns[1].as_map()[0].first.as_string(), "id");
    EXPECT_EQ((*rows)[1].value.as_string(), "two");

    Row probe = make_complex_row(2, "");
    auto found = reader->get(probe.key_columns, Version{.major = 10, .minor = 2});
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->value.as_string(), "two");
}

TEST(SSTableTest, BuildsAndReadsMultiLevelIndex) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), options);

    for (uint64_t i = 0; i < 18; ++i) {
        ASSERT_TRUE(builder
                        .add(make_row("tenant",
                                      i,
                                      Version{.major = 10, .minor = i},
                                      std::string("v") + std::to_string(i)))
                        .ok());
    }

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->statistics().data_block_count, 9u);
    EXPECT_GT(reader->statistics().index_block_count, 1u);

    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 18u);
    EXPECT_EQ(rows->front().value.as_string(), "v0");
    EXPECT_EQ(rows->back().value.as_string(), "v17");

    auto found =
        reader->get({Value::make<DataType::kString>("tenant"), Value::make<DataType::kUint64>(13)},
                    Version{.major = 10, .minor = 13});
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->value.as_string(), "v13");
}

TEST(SSTableTest, DataBlockSoftLimitFlushesAndHardLimitAllowsSingleRow) {
    BuilderOptions soft_options;
    soft_options.configuration.max_embedded_value_size = 1024;
    soft_options.configuration.max_data_block_size_soft_limit = block::Header::kSize;
    soft_options.configuration.max_data_block_size_hard_limit = 1024 * 1024;
    soft_options.configuration.max_data_block_row_count = 100;
    Builder soft_builder(make_schema(), soft_options);

    ASSERT_TRUE(soft_builder.add(make_row("a", 1, Version{.major = 10}, "one")).ok());
    ASSERT_TRUE(soft_builder.add(make_row("b", 2, Version{.major = 10}, "two")).ok());
    ASSERT_TRUE(soft_builder.add(make_row("c", 3, Version{.major = 10}, "three")).ok());
    auto files = soft_builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->statistics().data_block_count, 3u);

    // PDF §6.1: a single row that exceeds the hard limit is allowed as an exception.
    BuilderOptions hard_options;
    hard_options.configuration.max_embedded_value_size = 1024;
    hard_options.configuration.max_data_block_size_soft_limit = block::Header::kSize;
    hard_options.configuration.max_data_block_size_hard_limit = block::Header::kSize;
    Builder hard_builder(make_schema(), hard_options);
    ASSERT_TRUE(hard_builder.add(make_row("a", 1, Version{.major = 10}, "too-large")).ok());
    auto hard_files = hard_builder.finish();
    ASSERT_TRUE(hard_files.ok()) << hard_files.status();
    auto hard_reader = Reader::open(hard_files->key_file, hard_files->value_file);
    ASSERT_TRUE(hard_reader.ok()) << hard_reader.status();
    EXPECT_EQ(hard_reader->statistics().data_block_count, 1u);
}

TEST(SSTableTest, WritesIndexTreeInPostOrder) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), options);

    for (uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(builder
                        .add(make_row("tenant",
                                      i,
                                      Version{.major = 10, .minor = i},
                                      std::string("v") + std::to_string(i)))
                        .ok());
    }

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    const uint64_t root_offset = root_index_offset(files->key_file);
    const auto kinds = block_kinds_before_root(files->key_file, root_offset);
    const std::vector<block::Kind> expected{
        block::Kind::kData,
        block::Kind::kData,
        block::Kind::kIndex,
        block::Kind::kData,
        block::Kind::kData,
        block::Kind::kIndex,
    };
    EXPECT_EQ(kinds, expected);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(files->key_file, root_offset)),
              block::Kind::kRootIndex);

    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->statistics().data_block_count, 4u);
    EXPECT_EQ(reader->statistics().index_block_count, 3u);
    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 8u);
    EXPECT_EQ(rows->back().value.as_string(), "v7");
}

} // namespace
} // namespace pl::sstv2::file
