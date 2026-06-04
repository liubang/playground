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
#include <cstdint>
#include <cstdio>
#include <string>

#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/sstable_builder.h"
#include "cpp/pl/sstv2/file/sstable_reader.h"
#include "cpp/pl/sstv2/file/tail.h"
#include "cpp/pl/sstv2/file/value_file_reader.h"
#include "cpp/pl/sstv2/file/value_file_writer.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::file;

// =============================================================================
// LocatorTest
// =============================================================================

TEST(LocatorTest, RoundTrip) {
    Locator loc;
    loc.add(kSectionDataBlocks, 0, 4096);
    loc.add(kSectionIndexBlocks, 4096, 512);
    loc.add(kSectionBloomFilter, 4608, 256);

    std::string serialized = loc.serialize();
    ASSERT_FALSE(serialized.empty());

    auto result = Locator::deserialize(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(serialized.data()), serialized.size()));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& deserialized = *result;
    ASSERT_EQ(deserialized.entries().size(), 3u);

    EXPECT_EQ(deserialized.entries()[0].section_type, kSectionDataBlocks);
    EXPECT_EQ(deserialized.entries()[0].offset, 0u);
    EXPECT_EQ(deserialized.entries()[0].size, 4096u);

    EXPECT_EQ(deserialized.entries()[1].section_type, kSectionIndexBlocks);
    EXPECT_EQ(deserialized.entries()[1].offset, 4096u);
    EXPECT_EQ(deserialized.entries()[1].size, 512u);

    EXPECT_EQ(deserialized.entries()[2].section_type, kSectionBloomFilter);
    EXPECT_EQ(deserialized.entries()[2].offset, 4608u);
    EXPECT_EQ(deserialized.entries()[2].size, 256u);
}

TEST(LocatorTest, Find) {
    Locator loc;
    loc.add(kSectionDataBlocks, 100, 200);
    loc.add(kSectionBloomFilter, 300, 400);

    auto found = loc.find(kSectionBloomFilter);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->section_type, kSectionBloomFilter);
    EXPECT_EQ(found->offset, 300u);
    EXPECT_EQ(found->size, 400u);

    auto not_found = loc.find(kSectionIndexBlocks);
    EXPECT_FALSE(not_found.has_value());
}

TEST(LocatorTest, Empty) {
    Locator loc;
    std::string serialized = loc.serialize();

    auto result = Locator::deserialize(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(serialized.data()), serialized.size()));
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->entries().empty());
}

// =============================================================================
// TailTest
// =============================================================================

TEST(TailTest, RoundTrip) {
    Tail tail;
    tail.magic = 0x53535432;
    tail.format_version = 1;
    tail.reserved = 0;
    tail.locator_offset = 123456;
    tail.locator_size = 789;
    tail.locator_checksum = 0xDEADBEEF;
    tail.file_checksum = 0x1234567890ABCDEF;

    std::byte buf[Tail::kSize] = {};
    tail.encode_to(buf);

    auto result = Tail::decode_from(std::span<const std::byte>(buf, Tail::kSize));
    ASSERT_TRUE(result.ok()) << result.status();

    auto& decoded = *result;
    EXPECT_EQ(decoded.magic, 0x53535432u);
    EXPECT_EQ(decoded.format_version, 1u);
    EXPECT_EQ(decoded.reserved, 0u);
    EXPECT_EQ(decoded.locator_offset, 123456u);
    EXPECT_EQ(decoded.locator_size, 789u);
    EXPECT_EQ(decoded.locator_checksum, 0xDEADBEEFu);
    EXPECT_EQ(decoded.file_checksum, 0x1234567890ABCDEFu);
}

TEST(TailTest, InvalidMagic) {
    Tail tail;
    tail.magic = 0x12345678; // wrong magic

    std::byte buf[Tail::kSize] = {};
    tail.encode_to(buf);

    auto result = Tail::decode_from(std::span<const std::byte>(buf, Tail::kSize));
    EXPECT_FALSE(result.ok());
}

TEST(TailTest, TooShort) {
    std::byte buf[16] = {}; // less than kSize (32)
    auto result = Tail::decode_from(std::span<const std::byte>(buf, 16));
    EXPECT_FALSE(result.ok());
}

// =============================================================================
// ValueHandleTest
// =============================================================================

TEST(ValueHandleTest, RoundTrip) {
    ValueHandle handle;
    handle.file_id = 42;
    handle.offset = 1024;
    handle.size = 256;
    handle.checksum = 0xCAFEBABE;

    std::byte buf[ValueHandle::kSerializedSize] = {};
    handle.encode(buf);

    ValueHandle decoded = ValueHandle::decode(buf);
    EXPECT_EQ(decoded.file_id, 42u);
    EXPECT_EQ(decoded.offset, 1024u);
    EXPECT_EQ(decoded.size, 256u);
    EXPECT_EQ(decoded.checksum, 0xCAFEBABEu);
}

// =============================================================================
// ValueFileWriterReaderTest
// =============================================================================

TEST(ValueFileWriterReaderTest, WriteAndRead) {
    std::string temp_path = "/tmp/sstv2_ut_value_file.dat";

    ValueFileWriter writer(1, temp_path);

    std::string val1 = "hello world";
    std::string val2 = "another value with more data";
    std::string val3 = "short";

    auto h1 = writer.append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(val1.data()), val1.size()));
    ASSERT_TRUE(h1.ok()) << h1.status();

    auto h2 = writer.append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(val2.data()), val2.size()));
    ASSERT_TRUE(h2.ok()) << h2.status();

    auto h3 = writer.append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(val3.data()), val3.size()));
    ASSERT_TRUE(h3.ok()) << h3.status();

    ASSERT_TRUE(writer.finish().ok());

    auto reader_or = ValueFileReader::open(temp_path);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    auto& reader = *reader_or;

    auto r1 = reader.read(*h1);
    ASSERT_TRUE(r1.ok()) << r1.status();
    EXPECT_EQ(*r1, val1);

    auto r2 = reader.read(*h2);
    ASSERT_TRUE(r2.ok()) << r2.status();
    EXPECT_EQ(*r2, val2);

    auto r3 = reader.read(*h3);
    ASSERT_TRUE(r3.ok()) << r3.status();
    EXPECT_EQ(*r3, val3);

    std::remove(temp_path.c_str());
}

TEST(ValueFileWriterReaderTest, ChecksumValidation) {
    std::string temp_path = "/tmp/sstv2_ut_value_file_cksum.dat";

    ValueFileWriter writer(2, temp_path);

    std::string val = "checksum test data";
    auto h = writer.append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(val.data()), val.size()));
    ASSERT_TRUE(h.ok()) << h.status();
    ASSERT_TRUE(writer.finish().ok());

    // Corrupt the checksum in the handle
    ValueHandle corrupted = *h;
    corrupted.checksum ^= 0xFFFFFFFF;

    auto reader_or = ValueFileReader::open(temp_path);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();

    auto result = reader_or->read(corrupted);
    EXPECT_FALSE(result.ok());

    std::remove(temp_path.c_str());
}

// =============================================================================
// SSTableBuilderReaderTest
// =============================================================================

TEST(SSTableBuilderReaderTest, EndToEnd) {
    std::string temp_path = "/tmp/sstv2_ut_sstable.sst";

    SSTableBuilder::Options opts;
    SSTableBuilder builder(temp_path, opts);

    ASSERT_TRUE(builder.add("key_001", "value_001").ok());
    ASSERT_TRUE(builder.add("key_002", "value_002").ok());
    ASSERT_TRUE(builder.add("key_003", "value_003").ok());
    ASSERT_TRUE(builder.finish().ok());

    auto reader_or = SSTableReader::open(temp_path);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    auto& reader = *reader_or;

    EXPECT_TRUE(reader.is_valid());
    EXPECT_EQ(reader.tail().magic, 0x53535432u);
    EXPECT_EQ(reader.tail().format_version, 1u);

    // Locator should have at least a data blocks section
    auto data_section = reader.locator().find(kSectionDataBlocks);
    EXPECT_TRUE(data_section.has_value());

    std::remove(temp_path.c_str());
}

TEST(SSTableBuilderReaderTest, AbortPreventsFinish) {
    std::string temp_path = "/tmp/sstv2_ut_sstable_abort.sst";

    SSTableBuilder::Options opts;
    SSTableBuilder builder(temp_path, opts);

    ASSERT_TRUE(builder.add("key_a", "value_a").ok());
    builder.abort();

    // After abort, finish should fail or the file should not be valid
    auto status = builder.finish();
    EXPECT_FALSE(status.ok());

    std::remove(temp_path.c_str());
}
