// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <cstdio>
#include <span>
#include <string>

#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/sstable_builder.h"
#include "cpp/pl/sstv2/file/sstable_reader.h"
#include "cpp/pl/sstv2/file/tail.h"
#include "cpp/pl/sstv2/types/data_type.h"
#include "cpp/pl/sstv2/types/table_schema.h"
#include "cpp/pl/sstv2/types/variant.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::file;
using pl::sstv2::types::KeyColumn;
using pl::sstv2::types::Row;
using pl::sstv2::types::StructuredRowKey;
using pl::sstv2::types::TableSchema;

namespace {

std::span<const std::byte> Bytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

TableSchema TestSchema() {
    return TableSchema({
        KeyColumn{.name = "Key1", .type = pl::sstv2::types::DataType::kUint32},
        KeyColumn{.name = "Key2", .type = pl::sstv2::types::DataType::kString},
    });
}

Row MakeRow(uint32_t key1, std::string key2, uint64_t version, std::string value) {
    Row row;
    row.row_key = StructuredRowKey({
        pl::sstv2::types::Variant::uint32(key1),
        pl::sstv2::types::Variant::string(key2),
    });
    row.version = version;
    row.op_type = 0;
    row.value = pl::sstv2::types::Variant::string(value);
    return row;
}

} // namespace

TEST(LocatorTest, PdfMapRoundTrip) {
    Locator loc;
    loc.set_section("RootIndex", SectionLocation{13000, 1000});
    loc.set_section("Configuration", SectionLocation{25000, 1000});

    std::string encoded = loc.encode();
    ASSERT_GE(encoded.size(), 12u);

    auto decoded = Locator::decode(Bytes(encoded));
    ASSERT_TRUE(decoded.ok()) << decoded.status();

    auto root = decoded->find_section("RootIndex");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->offset, 13000u);
    EXPECT_EQ(root->length, 1000u);

    auto configuration = decoded->find_section("Configuration");
    ASSERT_TRUE(configuration.has_value());
    EXPECT_EQ(configuration->offset, 25000u);
    EXPECT_EQ(configuration->length, 1000u);
}

TEST(TailTest, PdfTailRoundTrip) {
    Tail tail;
    tail.locator_offset = 123456;
    tail.locator_length = 789;

    auto encoded = tail.encode();
    auto decoded = Tail::decode(std::span<const std::byte>(encoded.data(), encoded.size()));
    ASSERT_TRUE(decoded.ok()) << decoded.status();

    EXPECT_EQ(decoded->magic, Tail::kMagic);
    EXPECT_EQ(decoded->version, Tail::kVersion);
    EXPECT_EQ(decoded->locator_offset, 123456u);
    EXPECT_EQ(decoded->locator_length, 789u);
}

TEST(SSTableBuilderReaderTest, EmbeddedValuesEndToEnd) {
    std::string key_path = "/tmp/sstv2_pdf_embedded.sst";
    std::remove(key_path.c_str());

    SSTableBuilder builder(TestSchema(), key_path);
    ASSERT_TRUE(builder.add(MakeRow(1, "abc", 100, "xyz")).ok());
    ASSERT_TRUE(builder.add(MakeRow(2, "abc", 90, "hello")).ok());
    ASSERT_TRUE(builder.finish().ok());

    auto reader_or = SSTableReader::open(key_path);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    const auto& reader = *reader_or;

    EXPECT_TRUE(reader.is_valid());
    EXPECT_EQ(reader.tail().magic, Tail::kMagic);
    EXPECT_EQ(reader.tail().version, Tail::kVersion);
    EXPECT_TRUE(reader.locator().find_section("RootIndex").has_value());
    EXPECT_TRUE(reader.locator().find_section("BloomFilter0").has_value());
    EXPECT_TRUE(reader.locator().find_section("Configuration").has_value());
    EXPECT_TRUE(reader.locator().find_section("Schema").has_value());

    auto rows = reader.scan();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].embedded);
    EXPECT_EQ(rows[0].value_type, pl::sstv2::types::DataType::kString);
    auto v0 = reader.read_value_bytes(rows[0]);
    ASSERT_TRUE(v0.ok()) << v0.status();
    EXPECT_EQ(*v0, "xyz");
    auto bloom_hit = reader.may_contain_encoded_key(rows[0].all_key);
    ASSERT_TRUE(bloom_hit.ok()) << bloom_hit.status();
    EXPECT_TRUE(*bloom_hit);

    auto v1 = reader.read_value_bytes(rows[1]);
    ASSERT_TRUE(v1.ok()) << v1.status();
    EXPECT_EQ(*v1, "hello");

    std::remove(key_path.c_str());
}

TEST(SSTableBuilderReaderTest, SeparatesLargeValues) {
    std::string key_path = "/tmp/sstv2_pdf_separated.sst";
    std::string value_path = "/tmp/sstv2_pdf_separated.vlog";
    std::remove(key_path.c_str());
    std::remove(value_path.c_str());

    SSTableBuilder::Options opts;
    opts.max_embedded_value_size = 3;
    opts.value_file_path = value_path;

    SSTableBuilder builder(TestSchema(), key_path, opts);
    ASSERT_TRUE(builder.add(MakeRow(1, "abc", 100, "large-value")).ok());
    ASSERT_TRUE(builder.finish().ok());

    auto reader_or = SSTableReader::open(key_path);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    auto rows = reader_or->scan();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].embedded);
    EXPECT_EQ(rows[0].filename, value_path);

    auto value = reader_or->read_value_bytes(rows[0]);
    ASSERT_TRUE(value.ok()) << value.status();
    EXPECT_EQ(*value, "large-value");

    std::remove(key_path.c_str());
    std::remove(value_path.c_str());
}
