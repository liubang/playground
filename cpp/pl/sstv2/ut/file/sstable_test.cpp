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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/file/sstable.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/format/tail.h"
#include "cpp/pl/sstv2/io/local_filesystem.h"
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

std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

class TestFileSystem final : public io::FileSystem {
public:
    explicit TestFileSystem(bool fail_reads = false, uint64_t max_write_bytes = UINT64_MAX)
        : fail_reads_(fail_reads), max_write_bytes_(max_write_bytes) {}

    absl::StatusOr<io::FileHandle> create(std::string_view path,
                                          const io::CreateOptions& options = {}) override {
        return local_.create(path, options);
    }
    absl::StatusOr<io::FileHandle> open(std::string_view path) override {
        return local_.open(path);
    }
    absl::Status append(io::FileHandle handle, std::span<const std::byte> data) override {
        auto current = local_.size(handle);
        if (!current.ok()) {
            return current.status();
        }
        if (*current + data.size() > max_write_bytes_) {
            return absl::InternalError("injected append failure");
        }
        return local_.append(handle, data);
    }
    absl::Status read_at(io::FileHandle handle,
                         uint64_t offset,
                         std::span<std::byte> destination) override {
        ++read_count_;
        bytes_read_ += destination.size();
        max_read_size_ = std::max<uint64_t>(max_read_size_, destination.size());
        if (fail_reads_ && !destination.empty()) {
            return absl::DataLossError("injected read_at failure");
        }
        return local_.read_at(handle, offset, destination);
    }
    absl::StatusOr<uint64_t> size(io::FileHandle handle) override { return local_.size(handle); }
    absl::StatusOr<io::FileIdentity> close(io::FileHandle handle) override {
        return local_.close(handle);
    }
    absl::Status remove(std::string_view path) override { return local_.remove(path); }
    absl::Status rename(std::string_view source, std::string_view destination) override {
        return local_.rename(source, destination);
    }
    uint64_t read_count() const { return read_count_; }
    uint64_t bytes_read() const { return bytes_read_; }
    uint64_t max_read_size() const { return max_read_size_; }

private:
    io::LocalFileSystem local_;
    bool fail_reads_ = false;
    uint64_t max_write_bytes_ = UINT64_MAX;
    uint64_t read_count_ = 0;
    uint64_t bytes_read_ = 0;
    uint64_t max_read_size_ = 0;
};

io::FileHandle create_file(const std::shared_ptr<io::FileSystem>& filesystem,
                           std::string_view path) {
    auto handle = filesystem->create(path);
    EXPECT_TRUE(handle.ok()) << handle.status();
    return handle.ok() ? *handle : io::kInvalidFileHandle;
}

io::FileHandle write_and_open(const std::shared_ptr<io::FileSystem>& filesystem,
                              std::string_view path,
                              std::string_view data) {
    auto writer = filesystem->create(path, io::CreateOptions{.overwrite = true});
    EXPECT_TRUE(writer.ok()) << writer.status();
    if (!writer.ok()) {
        return io::kInvalidFileHandle;
    }
    EXPECT_TRUE(filesystem->append(*writer, bytes(data)).ok());
    EXPECT_TRUE(filesystem->close(*writer).ok());
    auto reader = filesystem->open(path);
    EXPECT_TRUE(reader.ok()) << reader.status();
    return reader.ok() ? *reader : io::kInvalidFileHandle;
}

std::string read_contents(const std::shared_ptr<io::FileSystem>& filesystem,
                          io::FileHandle handle) {
    auto length = filesystem->size(handle);
    EXPECT_TRUE(length.ok()) << length.status();
    if (!length.ok()) {
        return {};
    }
    std::string result(static_cast<size_t>(*length), '\0');
    EXPECT_TRUE(filesystem->read_at(handle, 0, std::as_writable_bytes(std::span(result))).ok());
    return result;
}

class SSTableTest : public ::testing::Test {
protected:
    struct PendingTable {
        io::FileHandle key_writer;
        io::FileHandle value_writer;
        std::string key_path;
        std::string value_path;
        bool finished = false;
    };

    struct Table {
        io::FileHandle key;
        io::FileHandle value;
        uint64_t key_size;
        uint64_t value_size;
        std::string key_file;
        std::string value_file;
    };

    void SetUp() override {
        root_ = std::filesystem::path(::testing::TempDir()) /
                ("sstv2_sstable_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        filesystem_ = std::make_shared<io::LocalFileSystem>();
    }

    void TearDown() override {
        filesystem_.reset();
        std::filesystem::remove_all(root_);
    }

    std::string path(std::string_view name) {
        return (root_ / (std::to_string(next_path_++) + "_" + std::string(name))).string();
    }

    Sinks make_sinks() {
        const std::string key_path = path("key.sstv2");
        const std::string value_path = path("value.sstv2");
        const auto key = create_file(filesystem_, key_path);
        const auto value = create_file(filesystem_, value_path);
        pending_.push_back(PendingTable{key, value, key_path, value_path});
        return Sinks{.filesystem = filesystem_, .key = key, .value = value};
    }

    absl::StatusOr<Table> finish(Builder& builder) {
        auto result = builder.finish_result();
        if (!result.ok()) {
            return result.status();
        }
        auto it = std::find_if(pending_.rbegin(), pending_.rend(), [](const PendingTable& item) {
            return !item.finished;
        });
        if (it == pending_.rend()) {
            return absl::NotFoundError("test builder has no registered sinks");
        }
        it->finished = true;
        auto key = filesystem_->open(it->key_path);
        if (!key.ok()) {
            return key.status();
        }
        auto value = filesystem_->open(it->value_path);
        if (!value.ok()) {
            return value.status();
        }
        return Table{*key,
                     *value,
                     result->key_file.length,
                     result->value_file.length,
                     read_contents(filesystem_, *key),
                     read_contents(filesystem_, *value)};
    }

    absl::StatusOr<Reader> open(const Table& table) {
        return Reader::open(filesystem_, table.key, table.value);
    }

    std::shared_ptr<io::LocalFileSystem> filesystem_;
    std::filesystem::path root_;
    uint64_t next_path_ = 0;
    std::vector<PendingTable> pending_;
};

TEST_F(SSTableTest, BuildOpenAndScanSeparatedValues) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 1;
    Builder builder(make_schema(), make_sinks(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 2, Version{.major = 9, .minor = 0}, "second")).ok());

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    ASSERT_FALSE(files->key_file.empty());
    ASSERT_EQ(files->value_file, "firstsecond");

    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->schema()->row_key_column_count(), 2u);
    EXPECT_EQ(reader->statistics().total_row_count, 2u);
    EXPECT_EQ(reader->statistics().data_block_count, 2u);
    EXPECT_EQ(reader->statistics().index_block_count, 1u);
    EXPECT_EQ(reader->statistics().key_file_size, files->key_size);
    EXPECT_EQ(reader->statistics().value_file_size, files->value_size);

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

TEST_F(SSTableTest, BuildOpenAndScanEmbeddedValues) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 64;
    Builder builder(make_schema(), make_sinks(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "tiny")).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    EXPECT_TRUE(files->value_file.empty());

    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();
    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].value.as_string(), "tiny");
}

TEST_F(SSTableTest, BoolValuesAreStoredInColumnFlagOnly) {
    Builder builder(make_schema(), make_sinks());

    ASSERT_TRUE(builder.add(make_bool_row("a", 1, Version{.major = 10, .minor = 0}, true)).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    EXPECT_TRUE(files->value_file.empty());

    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();
    auto rows = reader->scan();
    ASSERT_TRUE(rows.ok()) << rows.status();
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].value.type(), DataType::kBool);
    EXPECT_TRUE((*rows)[0].value.as_bool());
}

TEST_F(SSTableTest, RejectsOutOfOrderRows) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("b", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    EXPECT_FALSE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "second")).ok());
}

TEST_F(SSTableTest, ScanRejectsValueChecksumMismatch) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10, .minor = 0}, "first")).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    files->value_file[0] ^= 0x01;
    const auto corrupt_value =
        write_and_open(filesystem_, path("corrupt-value.sstv2"), files->value_file);

    auto reader = Reader::open(filesystem_, files->key, corrupt_value);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_FALSE(reader->scan().ok());
}

TEST_F(SSTableTest, OpenRejectsStatisticsFileSizeMismatch) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    Builder builder(make_schema(), make_sinks(), options);
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    const auto mismatched_value =
        write_and_open(filesystem_, path("mismatched-value.sstv2"), files->value_file + "!");
    EXPECT_FALSE(Reader::open(filesystem_, files->key, mismatched_value).ok());
}

TEST_F(SSTableTest, OpenRejectsCorruptRootIndexAndBloom) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();

    std::string corrupt_root = files->key_file;
    corrupt_root[static_cast<size_t>(root_index_offset(corrupt_root) + block::Header::kSize)] ^=
        0x01;
    const auto corrupt_root_handle =
        write_and_open(filesystem_, path("corrupt-root.sstv2"), corrupt_root);
    EXPECT_FALSE(Reader::open(filesystem_, corrupt_root_handle, files->value).ok());

    std::string corrupt_bloom = files->key_file;
    const uint64_t bloom_offset = locator_uint64(corrupt_bloom, "BloomFilter0_Offset");
    const uint64_t bloom_length = locator_uint64(corrupt_bloom, "BloomFilter0_Length");
    ASSERT_GT(bloom_length, 0u);
    corrupt_bloom[static_cast<size_t>(bloom_offset + bloom_length - 1)] ^= 0x01;
    const auto corrupt_bloom_handle =
        write_and_open(filesystem_, path("corrupt-bloom.sstv2"), corrupt_bloom);
    EXPECT_FALSE(Reader::open(filesystem_, corrupt_bloom_handle, files->value).ok());
}

TEST_F(SSTableTest, ArrayAndMapKeysRoundTrip) {
    auto schema = SchemaBuilder()
                      .add_column("path", DataType::kArray)
                      .add_column("attrs", DataType::kMap)
                      .build();
    ASSERT_TRUE(schema.has_value());
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    Builder builder(std::make_shared<const Schema>(std::move(*schema)), make_sinks(), options);

    ASSERT_TRUE(builder.add(make_complex_row(1, "one")).ok());
    ASSERT_TRUE(builder.add(make_complex_row(2, "two")).ok());

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
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

TEST_F(SSTableTest, PrefixRangeScanAcrossBlocks) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), make_sinks(), options);

    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "a1")).ok());
    ASSERT_TRUE(builder.add(make_row("a", 2, Version{.major = 10}, "a2")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 1, Version{.major = 10}, "b1")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 2, Version{.major = 10}, "b2")).ok());
    ASSERT_TRUE(builder.add(make_row("c", 1, Version{.major = 10}, "c1")).ok());

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
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

TEST_F(SSTableTest, PrefixRangeScanWithFullRowKeyAndVersionPrefix) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    Builder builder(make_schema(), make_sinks(), options);

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

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
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

TEST_F(SSTableTest, PrefixRangeScanRejectsInvalidPrefixAndEmptyRange) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "a1")).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
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

TEST_F(SSTableTest, BuildsAndReadsMultiLevelIndex) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), make_sinks(), options);

    for (uint64_t i = 0; i < 18; ++i) {
        ASSERT_TRUE(builder
                        .add(make_row("tenant",
                                      i,
                                      Version{.major = 10, .minor = i},
                                      std::string("v") + std::to_string(i)))
                        .ok());
    }

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
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

TEST_F(SSTableTest, DataBlockSoftLimitFlushesAndHardLimitAllowsSingleRow) {
    BuilderOptions soft_options;
    soft_options.configuration.max_embedded_value_size = 1024;
    soft_options.configuration.max_data_block_size_soft_limit = block::Header::kSize;
    soft_options.configuration.max_data_block_size_hard_limit = 1024 * 1024;
    soft_options.configuration.max_data_block_row_count = 100;
    Builder soft_builder(make_schema(), make_sinks(), soft_options);

    ASSERT_TRUE(soft_builder.add(make_row("a", 1, Version{.major = 10}, "one")).ok());
    ASSERT_TRUE(soft_builder.add(make_row("b", 2, Version{.major = 10}, "two")).ok());
    ASSERT_TRUE(soft_builder.add(make_row("c", 3, Version{.major = 10}, "three")).ok());
    auto files = finish(soft_builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();
    EXPECT_EQ(reader->statistics().data_block_count, 3u);

    // PDF §6.1: a single row that exceeds the hard limit is allowed as an exception.
    BuilderOptions hard_options;
    hard_options.configuration.max_embedded_value_size = 1024;
    hard_options.configuration.max_data_block_size_soft_limit = block::Header::kSize;
    hard_options.configuration.max_data_block_size_hard_limit = block::Header::kSize;
    Builder hard_builder(make_schema(), make_sinks(), hard_options);
    ASSERT_TRUE(hard_builder.add(make_row("a", 1, Version{.major = 10}, "too-large")).ok());
    auto hard_files = finish(hard_builder);
    ASSERT_TRUE(hard_files.ok()) << hard_files.status();
    auto hard_reader = open(*hard_files);
    ASSERT_TRUE(hard_reader.ok()) << hard_reader.status();
    EXPECT_EQ(hard_reader->statistics().data_block_count, 1u);
}

TEST_F(SSTableTest, BuilderSinksParityAndIteratorBoundedRead) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;

    Builder local_builder(make_schema(), make_sinks(), options);

    auto sink_fs = std::make_shared<TestFileSystem>();
    const auto sink_key_path = path("instrumented-key.sstv2");
    const auto sink_value_path = path("instrumented-value.sstv2");
    auto key_sink = create_file(sink_fs, sink_key_path);
    auto value_sink = create_file(sink_fs, sink_value_path);
    Builder instrumented_builder(
        make_schema(), Sinks{.filesystem = sink_fs, .key = key_sink, .value = value_sink}, options);

    for (uint64_t i = 0; i < 12; ++i) {
        auto row = make_row(
            "tenant", i, Version{.major = 10, .minor = i}, std::string("v") + std::to_string(i));
        ASSERT_TRUE(local_builder.add(row).ok());
        ASSERT_TRUE(instrumented_builder.add(row).ok());
    }

    auto local_files = finish(local_builder);
    auto instrumented_files = instrumented_builder.finish_result();
    ASSERT_TRUE(local_files.ok()) << local_files.status();
    ASSERT_TRUE(instrumented_files.ok()) << instrumented_files.status();
    auto key_reader = sink_fs->open(sink_key_path);
    auto value_reader = sink_fs->open(sink_value_path);
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();
    ASSERT_TRUE(value_reader.ok()) << value_reader.status();
    auto external_key = read_contents(sink_fs, *key_reader);
    auto external_value = read_contents(sink_fs, *value_reader);
    EXPECT_EQ(local_files->key_file, external_key);
    EXPECT_EQ(local_files->value_file, external_value);

    auto read_fs = std::make_shared<TestFileSystem>();
    auto inst_key = write_and_open(read_fs, path("read-key.sstv2"), external_key);
    auto inst_value = write_and_open(read_fs, path("read-value.sstv2"), external_value);
    auto reader = Reader::open(read_fs, inst_key, inst_value);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto it = reader->new_iterator(ScanOptions{
        .start = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{4})}},
        .limit = KeyPrefix{.key_columns = {Value::make<DataType::kString>("tenant"),
                                           Value::make<DataType::kUint64>(uint64_t{8})}},
    });
    ASSERT_TRUE(it.ok()) << it.status();

    ASSERT_TRUE(it->SeekToFirst().ok());
    std::vector<uint64_t> ids;
    while (it->Valid()) {
        ids.push_back(it->row().all_key.column(1).as_uint64());
        ASSERT_TRUE(it->Next().ok());
    }
    EXPECT_EQ((std::vector<uint64_t>{4, 5, 6, 7}), ids);
    EXPECT_GT(read_fs->read_count(), 0u);
    EXPECT_LT(read_fs->bytes_read(), external_key.size() + external_value.size());
    EXPECT_LE(read_fs->max_read_size(), 512u);
}

TEST_F(SSTableTest, FinishResultDisambiguatesEmptyFilesAndSize) {
    auto filesystem = std::make_shared<TestFileSystem>();
    const auto key_path = path("finish-result-key.sstv2");
    const auto value_path = path("finish-result-value.sstv2");
    auto key_sink = create_file(filesystem, key_path);
    auto value_sink = create_file(filesystem, value_path);
    Builder builder(make_schema(),
                    Sinks{.filesystem = filesystem, .key = key_sink, .value = value_sink});

    auto result = builder.finish_result();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_GT(result->key_file.length, 0U);
    EXPECT_EQ(result->value_file.length, 0U);
    EXPECT_TRUE(result->key_file.checksum_valid);
    EXPECT_TRUE(result->value_file.checksum_valid);
    EXPECT_EQ(result->row_count, 0U);
    EXPECT_FALSE(result->min_key.has_value());
    EXPECT_FALSE(result->max_key.has_value());
    auto repeated = builder.finish_result();
    ASSERT_TRUE(repeated.ok());
    EXPECT_EQ(*repeated, *result);
    EXPECT_FALSE(filesystem->size(key_sink).ok());
    auto key_reader = filesystem->open(key_path);
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();
    EXPECT_FALSE(read_contents(filesystem, *key_reader).empty());
}

TEST_F(SSTableTest, FinishToSinksSupportsExternalRowsInput) {
    std::vector<Row> rows;
    rows.push_back(make_row("a", 1, Version{.major = 10}, "v1"));
    rows.push_back(make_row("a", 2, Version{.major = 10}, "v2"));

    auto filesystem = std::make_shared<TestFileSystem>();
    const auto key_path = path("external-rows-key.sstv2");
    const auto value_path = path("external-rows-value.sstv2");
    auto key_sink = create_file(filesystem, key_path);
    auto value_sink = create_file(filesystem, value_path);

    auto result = Builder::finish_to_sinks(
        make_schema(),
        Sinks{.filesystem = filesystem, .key = key_sink, .value = value_sink},
        BuilderOptions{},
        rows);
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(result->row_count, 2U);
    ASSERT_TRUE(result->min_key.has_value());
    ASSERT_TRUE(result->max_key.has_value());
    EXPECT_LT(*result->min_key, *result->max_key);
    auto key_reader = filesystem->open(key_path);
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();
    EXPECT_FALSE(read_contents(filesystem, *key_reader).empty());
}

TEST_F(SSTableTest, FileSystemCloseInvalidatesHandle) {
    auto filesystem = std::make_shared<TestFileSystem>();
    auto file = create_file(filesystem, path("closed-handle.sstv2"));
    ASSERT_TRUE(filesystem->append(file, std::as_bytes(std::span(std::string_view("data")))).ok());
    ASSERT_TRUE(filesystem->close(file).ok());
    EXPECT_FALSE(filesystem->size(file).ok());
    EXPECT_FALSE(filesystem->append(file, std::as_bytes(std::span(std::string_view("more")))).ok());
}

TEST_F(SSTableTest, ReaderOpenPropagatesShortReadOnTail) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();

    auto filesystem = std::make_shared<TestFileSystem>(true);
    auto short_key = write_and_open(filesystem, path("short-key.sstv2"), files->key_file);
    auto value = write_and_open(filesystem, path("short-value.sstv2"), files->value_file);
    auto reader = Reader::open(filesystem, short_key, value);
    EXPECT_FALSE(reader.ok());
}

TEST_F(SSTableTest, BuilderSinkFailurePropagates) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    auto filesystem = std::make_shared<TestFileSystem>(false, 0);
    auto key_sink = create_file(filesystem, path("failed-key.sstv2"));
    auto value_sink = create_file(filesystem, path("failed-value.sstv2"));
    Builder builder(make_schema(),
                    Sinks{.filesystem = filesystem, .key = key_sink, .value = value_sink},
                    options);

    ASSERT_FALSE(builder.add(make_row("a", 1, Version{.major = 10}, "first")).ok());
}

TEST_F(SSTableTest, IteratorSeekAndBoundary) {
    Builder builder(make_schema(), make_sinks());
    ASSERT_TRUE(builder.add(make_row("a", 1, Version{.major = 10}, "a1")).ok());
    ASSERT_TRUE(builder.add(make_row("a", 2, Version{.major = 10}, "a2")).ok());
    ASSERT_TRUE(builder.add(make_row("b", 1, Version{.major = 10}, "b1")).ok());
    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();

    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto it = reader->new_iterator();
    ASSERT_TRUE(it.ok()) << it.status();
    ASSERT_TRUE(it->SeekToFirst().ok());
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->row().all_key.column(0).as_string(), "a");
    EXPECT_EQ(it->row().all_key.column(1).as_uint64(), 1u);

    ASSERT_TRUE(it->Seek(KeyPrefix{.key_columns = {Value::make<DataType::kString>("a"),
                                                   Value::make<DataType::kUint64>(uint64_t{2})}})
                    .ok());
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->row().all_key.column(1).as_uint64(), 2u);
    ASSERT_TRUE(it->Next().ok());
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->row().all_key.column(0).as_string(), "b");
    ASSERT_TRUE(it->Next().ok());
    EXPECT_FALSE(it->Valid());
}

TEST_F(SSTableTest, IteratorStateIsBoundedForManySmallBlocks) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 1;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), make_sinks(), options);

    constexpr uint64_t kRowCount = 256;
    for (uint64_t i = 0; i < kRowCount; ++i) {
        ASSERT_TRUE(builder
                        .add(make_row("tenant",
                                      i,
                                      Version{.major = 10, .minor = i},
                                      std::string("v") + std::to_string(i)))
                        .ok());
    }

    auto files = finish(builder);
    ASSERT_TRUE(files.ok()) << files.status();
    auto reader = open(*files);
    ASSERT_TRUE(reader.ok()) << reader.status();

    auto it = reader->new_iterator();
    ASSERT_TRUE(it.ok()) << it.status();
    ASSERT_TRUE(it->SeekToFirst().ok());

    size_t max_slots = 0;
    size_t visited = 0;
    while (it->Valid()) {
        max_slots = std::max(max_slots, it->state_slots_for_test());
        ++visited;
        ASSERT_TRUE(it->Next().ok());
    }

    EXPECT_EQ(visited, kRowCount);
    EXPECT_LE(max_slots, 16u);
}

TEST_F(SSTableTest, WritesIndexTreeInPostOrder) {
    BuilderOptions options;
    options.configuration.max_embedded_value_size = 0;
    options.configuration.max_data_block_row_count = 2;
    options.configuration.max_index_block_row_count = 2;
    Builder builder(make_schema(), make_sinks(), options);

    for (uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(builder
                        .add(make_row("tenant",
                                      i,
                                      Version{.major = 10, .minor = i},
                                      std::string("v") + std::to_string(i)))
                        .ok());
    }

    auto files = finish(builder);
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

    auto reader = open(*files);
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
