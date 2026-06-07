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

#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/sstv2/format/metadata.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/format/tail.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::format {
namespace {

using types::DataType;
using types::SchemaBuilder;
using types::SortOrder;
using types::Value;
using types::Version;

TEST(TailTest, RoundTripAndRejectsCorruption) {
    const std::string encoded = encode_tail(Tail{.locator_offset = 4096, .locator_length = 128});
    ASSERT_EQ(encoded.size(), Tail::kSize);

    auto decoded = decode_tail(encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(decoded->locator_offset, 4096u);
    EXPECT_EQ(decoded->locator_length, 128u);
    EXPECT_EQ(decoded->version, Tail::kVersion);
    EXPECT_EQ(decoded->magic, Tail::kMagic);

    std::string corrupted = encoded;
    corrupted[9] ^= 0x7F;
    EXPECT_FALSE(decode_tail(corrupted).ok());
}

TEST(SectionTest, LocatorRoundTripAndRejectsCorruption) {
    SectionEntries entries;
    entries.emplace_back("RootIndex_Offset", Value::make<DataType::kUint64>(uint64_t{52}));
    entries.emplace_back("RootIndex_Length", Value::make<DataType::kUint64>(uint64_t{256}));
    entries.emplace_back("SchemaName", Value::make<DataType::kString>("orders"));
    entries.emplace_back("Version",
                         Value::make<DataType::kVersion>(Version{.major = 1, .minor = 2}));

    const std::string encoded =
        encode_section(SectionMagic::kLocator, make_section_map(std::move(entries)));
    auto decoded = decode_section(encoded, SectionMagic::kLocator);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    ASSERT_EQ(decoded->entries.as_map().size(), 4u);
    const Value* root_offset = find_section_value(decoded->entries, "RootIndex_Offset");
    const Value* root_length = find_section_value(decoded->entries, "RootIndex_Length");
    const Value* schema_name = find_section_value(decoded->entries, "SchemaName");
    const Value* version = find_section_value(decoded->entries, "Version");
    ASSERT_NE(root_offset, nullptr);
    ASSERT_NE(root_length, nullptr);
    ASSERT_NE(schema_name, nullptr);
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(root_offset->as_uint64(), 52u);
    EXPECT_EQ(root_length->as_uint64(), 256u);
    EXPECT_EQ(schema_name->as_string(), "orders");
    EXPECT_EQ(version->as_version().major, 1u);
    EXPECT_EQ(version->as_version().minor, 2u);

    std::string corrupted = encoded;
    corrupted.back() ^= 0x01;
    EXPECT_FALSE(decode_section(corrupted, SectionMagic::kLocator).ok());
}

TEST(MetadataTest, StrongSchemaRoundTrip) {
    auto schema = SchemaBuilder()
                      .add_column("tenant", DataType::kString)
                      .add_column("sequence", DataType::kUint64, SortOrder::kDescending)
                      .build();
    ASSERT_TRUE(schema.has_value());

    const std::string encoded = encode_schema(*schema);
    auto decoded = decode_schema(encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    ASSERT_NE(*decoded, nullptr);
    EXPECT_EQ((*decoded)->row_key_column_count(), 2u);
    EXPECT_EQ((*decoded)->column_name(0), "tenant");
    EXPECT_EQ((*decoded)->column_type(0), DataType::kString);
    EXPECT_EQ((*decoded)->column_order(1), SortOrder::kDescending);
}

TEST(MetadataTest, ConfigurationAndStatisticsRoundTrip) {
    const Configuration configuration{
        .max_embedded_value_size = 128,
        .max_data_block_size_soft_limit = 4096,
        .max_data_block_size_hard_limit = 8192,
        .max_data_block_row_count = 99,
    };
    auto decoded_configuration = decode_configuration(encode_configuration(configuration));
    ASSERT_TRUE(decoded_configuration.ok()) << decoded_configuration.status();
    EXPECT_EQ(decoded_configuration->max_embedded_value_size, 128u);
    EXPECT_EQ(decoded_configuration->max_data_block_row_count, 99u);

    const Statistics statistics{
        .total_row_count = 5,
        .data_block_count = 2,
        .index_block_count = 1,
        .key_file_size = 2048,
        .value_file_size = 512,
    };
    auto decoded_statistics = decode_statistics(encode_statistics(statistics));
    ASSERT_TRUE(decoded_statistics.ok()) << decoded_statistics.status();
    EXPECT_EQ(decoded_statistics->total_row_count, 5u);
    EXPECT_EQ(decoded_statistics->data_block_count, 2u);
    EXPECT_EQ(decoded_statistics->index_block_count, 1u);
}

} // namespace
} // namespace pl::sstv2::format
