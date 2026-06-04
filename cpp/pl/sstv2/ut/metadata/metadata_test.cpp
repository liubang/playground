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

#include <cstddef>
#include <string>

#include "cpp/pl/sstv2/metadata/compatibility.h"
#include "cpp/pl/sstv2/metadata/configuration.h"
#include "cpp/pl/sstv2/metadata/metadata_section.h"
#include "cpp/pl/sstv2/metadata/schema_meta.h"
#include "cpp/pl/sstv2/metadata/statistics.h"
#include "cpp/pl/sstv2/metadata/user_defined.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::metadata;

namespace {

constexpr uint32_t kTestMagic = 0x54455354; // "TEST"

std::span<const std::byte> as_span(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

} // namespace

// ===========================================================================
// MetadataSectionTest
// ===========================================================================

TEST(MetadataSectionTest, RoundTrip) {
    MetadataSection section;
    section.put("name", "test_table");
    section.put_uint64("row_count", 123456);
    section.put_uint32("block_count", 42);
    section.put_uint16("version", 7);

    std::string data = section.serialize(kTestMagic);
    ASSERT_FALSE(data.empty());

    auto result = MetadataSection::deserialize(as_span(data), kTestMagic);
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.get("name"), "test_table");
    EXPECT_EQ(restored.get_uint64("row_count"), 123456u);
    EXPECT_EQ(restored.get_uint32("block_count"), 42u);
    EXPECT_EQ(restored.get_uint16("version"), 7u);
}

TEST(MetadataSectionTest, Empty) {
    MetadataSection section;
    EXPECT_TRUE(section.empty());

    std::string data = section.serialize(kTestMagic);
    auto result = MetadataSection::deserialize(as_span(data), kTestMagic);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_TRUE(result->empty());
    EXPECT_EQ(result->size(), 0u);
}

TEST(MetadataSectionTest, Overwrite) {
    MetadataSection section;
    section.put("key", "first");
    section.put("key", "second");

    std::string data = section.serialize(kTestMagic);
    auto result = MetadataSection::deserialize(as_span(data), kTestMagic);
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_EQ(result->get("key"), "second");
}

TEST(MetadataSectionTest, InvalidMagic) {
    MetadataSection section;
    section.put("k", "v");

    std::string data = section.serialize(kTestMagic);
    auto result = MetadataSection::deserialize(as_span(data), 0xDEADBEEF);
    EXPECT_FALSE(result.ok());
}

TEST(MetadataSectionTest, Truncated) {
    MetadataSection section;
    section.put("key", "value");

    std::string data = section.serialize(kTestMagic);
    ASSERT_GT(data.size(), 4u);

    // Truncate to just a few bytes
    std::string truncated = data.substr(0, 4);
    auto result = MetadataSection::deserialize(as_span(truncated), kTestMagic);
    EXPECT_FALSE(result.ok());
}

TEST(MetadataSectionTest, ChecksumCorruption) {
    MetadataSection section;
    section.put("important", "data");

    std::string data = section.serialize(kTestMagic);
    ASSERT_GT(data.size(), 8u);

    // Flip a byte in the middle of the payload
    data[data.size() / 2] ^= 0xFF;

    auto result = MetadataSection::deserialize(as_span(data), kTestMagic);
    EXPECT_FALSE(result.ok());
}

// ===========================================================================
// ConfigurationTest
// ===========================================================================

TEST(ConfigurationTest, RoundTrip) {
    Configuration config;
    config.block_size = 131072;
    config.compression = 2;
    config.bloom_bits_per_key = 14;
    config.value_size_threshold = 2048;
    config.max_prefix_rounds = 8;

    std::string data = config.serialize();
    auto result = Configuration::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.block_size, 131072u);
    EXPECT_EQ(restored.compression, 2u);
    EXPECT_EQ(restored.bloom_bits_per_key, 14u);
    EXPECT_EQ(restored.value_size_threshold, 2048u);
    EXPECT_EQ(restored.max_prefix_rounds, 8u);
}

TEST(ConfigurationTest, Defaults) {
    Configuration config;
    std::string data = config.serialize();
    auto result = Configuration::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.block_size, 65536u);
    EXPECT_EQ(restored.compression, 0u);
    EXPECT_EQ(restored.bloom_bits_per_key, 10u);
    EXPECT_EQ(restored.value_size_threshold, 1024u);
    EXPECT_EQ(restored.max_prefix_rounds, 4u);
}

// ===========================================================================
// SchemaMetaTest
// ===========================================================================

TEST(SchemaMetaTest, RoundTrip) {
    SchemaMeta schema;
    schema.columns = {
        {"id", 1},
        {"name", 2},
        {"score", 3},
        {"ts", 4},
    };

    std::string data = schema.serialize();
    auto result = SchemaMeta::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    ASSERT_EQ(restored.columns.size(), 4u);
    EXPECT_EQ(restored.columns[0].name, "id");
    EXPECT_EQ(restored.columns[0].data_type, 1u);
    EXPECT_EQ(restored.columns[1].name, "name");
    EXPECT_EQ(restored.columns[1].data_type, 2u);
    EXPECT_EQ(restored.columns[2].name, "score");
    EXPECT_EQ(restored.columns[2].data_type, 3u);
    EXPECT_EQ(restored.columns[3].name, "ts");
    EXPECT_EQ(restored.columns[3].data_type, 4u);
}

TEST(SchemaMetaTest, EmptySchema) {
    SchemaMeta schema;
    std::string data = schema.serialize();
    auto result = SchemaMeta::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    EXPECT_TRUE(result->columns.empty());
}

// ===========================================================================
// StatisticsTest
// ===========================================================================

TEST(StatisticsTest, RoundTrip) {
    Statistics stats;
    stats.total_rows = 1000000;
    stats.total_data_blocks = 256;
    stats.total_index_blocks = 4;
    stats.raw_data_size = 67108864;
    stats.compressed_data_size = 33554432;
    stats.min_row_key = "aaa_first";
    stats.max_row_key = "zzz_last";
    stats.creation_time = 1717500000000000;

    std::string data = stats.serialize();
    auto result = Statistics::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.total_rows, 1000000u);
    EXPECT_EQ(restored.total_data_blocks, 256u);
    EXPECT_EQ(restored.total_index_blocks, 4u);
    EXPECT_EQ(restored.raw_data_size, 67108864u);
    EXPECT_EQ(restored.compressed_data_size, 33554432u);
    EXPECT_EQ(restored.min_row_key, "aaa_first");
    EXPECT_EQ(restored.max_row_key, "zzz_last");
    EXPECT_EQ(restored.creation_time, 1717500000000000u);
}

// ===========================================================================
// CompatibilityTest
// ===========================================================================

TEST(CompatibilityTest, RoundTrip) {
    Compatibility compat;
    compat.min_reader_version = 3;
    compat.writer_version = 5;
    compat.feature_flags = 0x0000000F;

    std::string data = compat.serialize();
    auto result = Compatibility::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.min_reader_version, 3u);
    EXPECT_EQ(restored.writer_version, 5u);
    EXPECT_EQ(restored.feature_flags, 0x0000000Fu);
}

// ===========================================================================
// UserDefinedTest
// ===========================================================================

TEST(UserDefinedTest, RoundTrip) {
    UserDefined ud;
    ud.section.put("author", "liubang");
    ud.section.put("description", "test sst file");
    ud.section.put_uint64("custom_id", 99999);

    std::string data = ud.serialize();
    auto result = UserDefined::deserialize(as_span(data));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& restored = *result;
    EXPECT_EQ(restored.section.get("author"), "liubang");
    EXPECT_EQ(restored.section.get("description"), "test sst file");
    EXPECT_EQ(restored.section.get_uint64("custom_id"), 99999u);
}
