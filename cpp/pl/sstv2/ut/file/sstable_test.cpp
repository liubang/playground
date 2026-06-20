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

#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/format/tail.h"
#include "cpp/pl/sstv2/types/row.h"

namespace pl::sstv2::file {
namespace {

using types::AllKey;
using types::DataType;
using types::OpType;
using types::Row;
using types::RowKey;
using types::Schema;
using types::SchemaBuilder;
using types::SystemKey;
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
    return Row::create(RowKey::from_columns({
                           Value::make<DataType::kString>(std::move(tenant)),
                           Value::make<DataType::kUint64>(id),
                       }),
                       SystemKey{version, OpType::kPut},
                       Value::make<DataType::kString>(std::move(value)));
}

Row make_bool_row(std::string tenant, uint64_t id, Version version, bool value) {
    return Row::create(RowKey::from_columns({
                           Value::make<DataType::kString>(std::move(tenant)),
                           Value::make<DataType::kUint64>(id),
                       }),
                       SystemKey{version, OpType::kPut},
                       Value::make<DataType::kBool>(value));
}

Row make_complex_row(uint64_t id, std::string value) {
    return Row::create(
        RowKey::from_columns({
            Value::make_array({
                Value::make<DataType::kString>("tenant"),
                Value::make<DataType::kUint64>(id),
            }),
            Value::make_map({
                {Value::make<DataType::kString>("id"), Value::make<DataType::kUint64>(id)},
                {Value::make<DataType::kString>("kind"), Value::make<DataType::kString>("event")},
            }),
        }),
        SystemKey{Version{.major = 10, .minor = id}, OpType::kPut},
        Value::make<DataType::kString>(std::move(value)));
}

uint64_t locator_uint64(std::string_view key_file, std::string_view name) {
    auto tail = format::decode_tail(key_file.substr(key_file.size() - format::Tail::kSize));
    EXPECT_TRUE(tail.ok()) << tail.status();
    auto locator =
        format::decode_section(key_file.substr(static_cast<size_t>(tail->locator_offset),
                                               static_cast<size_t>(tail->locator_length)),
                               format::SectionMagic::kLocator);
    EXPECT_TRUE(locator.ok()) << locator.status();
    const Value* value = format::find_section_value(locator->entries, name);
    EXPECT_NE(value, nullptr);
    EXPECT_EQ(value->type(), DataType::kUint64);
    return value->as_uint64();
}

uint64_t root_index_offset(std::string_view key_file) {
    return locator_uint64(key_file, "RootIndex_Offset");
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
    EXPECT_EQ((*rows)[0].all_key.column(0).as_string(), "a");
    EXPECT_EQ((*rows)[0].all_key.column(1).as_uint64(), 1u);
    EXPECT_EQ((*rows)[0].system_key().version.major, 10u);
    EXPECT_EQ((*rows)[0].value.as_string(), "first");
    EXPECT_EQ((*rows)[1].all_key.column(0).as_string(), "b");
    EXPECT_EQ((*rows)[1].value.as_string(), "second");

    auto found = reader->get(RowKey::from_columns({
                                 Value::make<DataType::kString>("b"),
                                 Value::make<DataType::kUint64>(2),
                             }),
                             SystemKey{Version{.major = 9, .minor = 0}});
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->value.as_string(), "second");

    auto missing = reader->get(RowKey::from_columns({
                                   Value::make<DataType::kString>("z"),
                                   Value::make<DataType::kUint64>(9),
                               }),
                               SystemKey{Version{.major = 1, .minor = 0}});
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

TEST(SSTableTest, OpenRejectsStatisticsFileSizeMismatch) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    Builder builder(make_schema(), options);
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    EXPECT_FALSE(Reader::open(files->key_file, files->value_file + "!").ok());
}

TEST(SSTableTest, OpenRejectsCorruptRootIndexAndBloom) {
    Builder builder(make_schema());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();

    std::string corrupt_root = files->key_file;
    corrupt_root[static_cast<size_t>(root_index_offset(corrupt_root) + block::Header::kSize)] ^=
        0x01;
    EXPECT_FALSE(Reader::open(corrupt_root, files->value_file).ok());

    std::string corrupt_bloom = files->key_file;
    const uint64_t bloom_offset = locator_uint64(corrupt_bloom, "BloomFilter0_Offset");
    const uint64_t bloom_length = locator_uint64(corrupt_bloom, "BloomFilter0_Length");
    ASSERT_GT(bloom_length, 0u);
    corrupt_bloom[static_cast<size_t>(bloom_offset + bloom_length - 1)] ^= 0x01;
    EXPECT_FALSE(Reader::open(corrupt_bloom, files->value_file).ok());
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
    EXPECT_EQ((*rows)[0].all_key.column(0).as_array()[1].as_uint64(), 1u);
    EXPECT_EQ((*rows)[1].all_key.column(1).as_map()[0].first.as_string(), "id");
    EXPECT_EQ((*rows)[1].value.as_string(), "two");

    auto found = reader->get(
        RowKey::from_columns({
            Value::make_array({
                Value::make<DataType::kString>("tenant"),
                Value::make<DataType::kUint64>(uint64_t{2}),
            }),
            Value::make_map({
                {Value::make<DataType::kString>("id"), Value::make<DataType::kUint64>(uint64_t{2})},
                {Value::make<DataType::kString>("kind"), Value::make<DataType::kString>("event")},
            }),
        }),
        SystemKey{Version{.major = 10, .minor = 2}});
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->value.as_string(), "two");
}

TEST(SSTableTest, PrefixRangeScanAcrossBlocks) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "a1")).ok());
    ASSERT_TRUE(builder.add(make_row("a", 2, Version{.major = 10}, "a2")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 1, Version{.major = 10}, "b1")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 2, Version{.major = 10}, "b2")).ok());
    ASSERT_TRUE(builder.add(make_row("c", 1, Version{.major = 10}, "c1")).ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto rows = reader->scan(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("b")}},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("c")}},
    });
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 2u);
    EXPECT_EQ((*rows)[0].value.as_string(), "b1");
    EXPECT_EQ((*rows)[1].value.as_string(), "b2");
}

TEST(SSTableTest, PrefixRangeScanWithFullRowKeyAndVersionPrefix) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    Builder builder(make_schema(), options);

    for (uint64_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(
            builder.add(make_row("tenant", i, Version{.major = 10}, absl::StrCat("v", i))).ok());
    }

    auto versioned = make_row("tenant", 6, Version{.major = 10}, "v10");
    ASSERT_TRUE(builder.add(std::move(versioned)).ok());
    ASSERT_TRUE(builder.add(make_row("tenant", 6, Version{.major = 9}, "v9")).ok());
    ASSERT_TRUE(builder.add(make_row("tenant", 6, Version{.major = 8}, "v8")).ok());
    ASSERT_TRUE(builder.add(make_row("tenant", 7, Version{.major = 10}, "put")).ok());
    ASSERT_TRUE(builder
                    .add(Row::create(RowKey::from_columns({
                                         Value::make<DataType::kString>("tenant"),
                                         Value::make<DataType::kUint64>(uint64_t{7}),
                                     }),
                                     SystemKey{Version{.major = 10}, OpType::kMerge},
                                     Value::make<DataType::kString>("merge")))
                    .ok());
    ASSERT_TRUE(builder
                    .add(Row::create(RowKey::from_columns({
                                         Value::make<DataType::kString>("tenant"),
                                         Value::make<DataType::kUint64>(uint64_t{7}),
                                     }),
                                     SystemKey{Version{.major = 10}, OpType::kDelete}))
                    .ok());

    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto rowkey_rows = reader->scan(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{2})}},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{4})}},
    });
    ASSERT_TRUE(rowkey_rows.ok()) << rowkey_rows.status();
    ASSERT_EQ(rowkey_rows->size(), 2u);
    EXPECT_EQ((*rowkey_rows)[0].all_key.column(1).as_uint64(), 2u);
    EXPECT_EQ((*rowkey_rows)[1].all_key.column(1).as_uint64(), 3u);

    auto version_rows = reader->scan(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{6})},
                           .version = Version{.major = 9}},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{6})},
                           .version = Version{.major = 8}},
    });
    ASSERT_TRUE(version_rows.ok()) << version_rows.status();
    ASSERT_EQ(version_rows->size(), 1u);
    EXPECT_EQ((*version_rows)[0].system_key().version.major, 9u);
    EXPECT_EQ((*version_rows)[0].value.as_string(), "v9");

    auto op_rows = reader->scan(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{7})},
                           .version = Version{.major = 10},
                           .op_type = OpType::kMerge},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{7})},
                           .version = Version{.major = 10},
                           .op_type = OpType::kDelete},
    });
    ASSERT_TRUE(op_rows.ok()) << op_rows.status();
    ASSERT_EQ(op_rows->size(), 1u);
    EXPECT_EQ((*op_rows)[0].system_key().op_type, OpType::kMerge);
    EXPECT_EQ((*op_rows)[0].value.as_string(), "merge");
}

TEST(SSTableTest, PrefixRangeScanRejectsInvalidPrefixAndEmptyRange) {
    Builder builder(make_schema());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "a1")).ok());
    auto files = builder.finish();
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = Reader::open(files->key_file, files->value_file);
    ASSERT_TRUE(reader.ok()) << reader.status();

    EXPECT_FALSE(reader
                     ->scan(ScanOptions{
                         .start =
                             KeyPrefix{
                                 .key_columns = {Value::make<DataType::kString>("a")},
                                 .version = Version{.major = 10},
                             },
                     })
                     .ok());
    EXPECT_FALSE(reader
                     ->scan(ScanOptions{
                         .start = KeyPrefix{.op_type = OpType::kPut},
                     })
                     .ok());

    auto empty = reader->scan(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("b")}},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("a")}},
    });
    ASSERT_TRUE(empty.ok()) << empty.status();
    EXPECT_TRUE(empty->empty());
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

    auto found = reader->get(RowKey::from_columns({
                                 Value::make<DataType::kString>("tenant"),
                                 Value::make<DataType::kUint64>(uint64_t{13}),
                             }),
                             SystemKey{Version{.major = 10, .minor = 13}});
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
