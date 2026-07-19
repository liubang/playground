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
// Created: 2026/06/07 00:00

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/index/index_tree.h"
#include "cpp/pl/sstv2/io/local_filesystem.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/key_factory.h"
#include "cpp/pl/sstv2/types/row.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::index {
namespace {

using types::DataType;
using types::InternalRow;
using types::InternalSchema;
using types::OpType;
using types::Schema;
using types::SchemaBuilder;
using types::Value;
using types::Version;

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder().add_column("id", DataType::kUint64).build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

InternalRow make_fence_row(const InternalSchema::ConstRef& schema, uint64_t id) {
    InternalRow row = InternalRow::make(schema);
    row.columns[0] = Value::make<DataType::kUint64>(id);
    row.columns[schema->version_index()] =
        Value::make<DataType::kVersion>(Version{.major = 10, .minor = id});
    row.columns[schema->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema->flag_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->filename_index()] = Value::make<DataType::kString>("@1");
    row.columns[schema->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->length_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    return row;
}

types::AllKey all_key_for(const InternalSchema::ConstRef& schema, const InternalRow& row) {
    auto all_key = types::make_all_key(row, schema);
    EXPECT_TRUE(all_key.ok()) << all_key.status();
    return std::move(*all_key);
}

types::PrefixKey prefix_key_for(const InternalSchema::ConstRef& schema, uint64_t id) {
    auto prefix =
        types::make_prefix_key(types::KeyPrefix{.key_columns = {Value::make<DataType::kUint64>(id)},
                                                .version = std::nullopt,
                                                .op_type = std::nullopt},
                               schema->user_schema(),
                               schema);
    EXPECT_TRUE(prefix.ok()) << prefix.status();
    return std::move(*prefix);
}

uint64_t block_length(std::string_view key_file, uint64_t offset) {
    const uint64_t uncompressed = codec::read_fixed64(key_file, offset + 28);
    const uint64_t compressed = codec::read_fixed64(key_file, offset + 36);
    return compressed == 0 ? uncompressed : compressed;
}

class ReadFailingFileSystem final : public io::FileSystem {
public:
    explicit ReadFailingFileSystem(std::shared_ptr<io::FileSystem> delegate)
        : delegate_(std::move(delegate)) {}

    [[nodiscard]] absl::StatusOr<io::FileHandle> create(
        std::string_view path, const io::CreateOptions& options = {}) override {
        return delegate_->create(path, options);
    }
    [[nodiscard]] absl::StatusOr<io::FileHandle> open(std::string_view path) override {
        return delegate_->open(path);
    }
    [[nodiscard]] absl::Status append(io::FileHandle handle,
                                      std::span<const std::byte> data) override {
        return delegate_->append(handle, data);
    }
    [[nodiscard]] absl::Status read_at(io::FileHandle, uint64_t, std::span<std::byte>) override {
        return absl::DataLossError("injected read_at failure");
    }
    [[nodiscard]] absl::StatusOr<uint64_t> size(io::FileHandle handle) override {
        return delegate_->size(handle);
    }
    [[nodiscard]] absl::StatusOr<io::FileIdentity> close(io::FileHandle handle) override {
        return delegate_->close(handle);
    }
    [[nodiscard]] absl::Status remove(std::string_view path) override {
        return delegate_->remove(path);
    }
    [[nodiscard]] absl::Status rename(std::string_view source,
                                      std::string_view destination) override {
        return delegate_->rename(source, destination);
    }

private:
    std::shared_ptr<io::FileSystem> delegate_;
};

class IndexTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = std::filesystem::path(::testing::TempDir()) /
                (std::string("sstv2_index_tree_") + info->test_suite_name() + "_" + info->name());
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        filesystem_ = std::make_shared<io::LocalFileSystem>();
    }

    void TearDown() override {
        filesystem_.reset();
        std::filesystem::remove_all(root_);
    }

    [[nodiscard]] std::string path(std::string_view name) const {
        return (root_ / std::filesystem::path(name)).string();
    }

    [[nodiscard]] io::FileHandle create_file(std::string_view name) {
        auto handle = filesystem_->create(path(name));
        EXPECT_TRUE(handle.ok()) << handle.status();
        return handle.ok() ? *handle : io::kInvalidFileHandle;
    }

    [[nodiscard]] std::string file_contents(std::string_view name) {
        auto handle = filesystem_->open(path(name));
        EXPECT_TRUE(handle.ok()) << handle.status();
        if (!handle.ok()) {
            return {};
        }
        auto size = filesystem_->size(*handle);
        EXPECT_TRUE(size.ok()) << size.status();
        if (!size.ok()) {
            static_cast<void>(filesystem_->close(*handle));
            return {};
        }
        std::string contents(*size, '\0');
        const auto read_status =
            filesystem_->read_at(*handle, 0, std::as_writable_bytes(std::span(contents)));
        EXPECT_TRUE(read_status.ok()) << read_status;
        EXPECT_TRUE(filesystem_->close(*handle).ok());
        return contents;
    }

    std::filesystem::path root_;
    std::shared_ptr<io::LocalFileSystem> filesystem_;
};

TEST_F(IndexTreeTest, BuildsPostOrderTreeAndRoutesToDataBlock) {
    auto schema = InternalSchema::make(make_schema());
    auto key_file = create_file("key");
    TreeBuilder builder(schema, 2, 4096, 8192, {}, filesystem_, key_file);

    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->block_count, 3u);

    const std::string key_bytes = file_contents("key");
    const uint64_t first_index = 0;
    const uint64_t second_index = block_length(key_bytes, first_index);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_bytes, first_index)),
              block::Kind::kIndex);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_bytes, second_index)),
              block::Kind::kIndex);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_bytes, result->root.offset)),
              block::Kind::kRootIndex);

    auto key_reader = filesystem_->open(path("key"));
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();

    std::vector<BlockRef> refs;
    ASSERT_TRUE(
        TreeReader::scan_data_blocks(filesystem_, *key_reader, schema, result->root, &refs).ok());
    ASSERT_EQ(refs.size(), 4u);
    EXPECT_EQ(refs[0].offset, 0u);
    EXPECT_EQ(refs[3].offset, 30u);

    auto found = TreeReader::find_data_block(filesystem_,
                                             *key_reader,
                                             schema,
                                             result->root,
                                             all_key_for(schema, make_fence_row(schema, 2)));
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->offset, 20u);
    EXPECT_EQ((*found)->length, 10u);
}

TEST_F(IndexTreeTest, RangeScanPrunesBlocksAfterLimitFence) {
    auto schema = InternalSchema::make(make_schema());
    auto key_file = create_file("key");
    TreeBuilder builder(schema, 2, 4096, 8192, {}, filesystem_, key_file);

    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();

    auto key_reader = filesystem_->open(path("key"));
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();

    std::vector<BlockRef> refs;
    const auto limit = prefix_key_for(schema, 2);
    ASSERT_TRUE(TreeReader::scan_data_blocks_in_range(filesystem_,
                                                      *key_reader,
                                                      schema,
                                                      result->root,
                                                      std::nullopt,
                                                      std::optional<types::PrefixKey>{limit},
                                                      &refs)
                    .ok());
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].offset, 0u);
    EXPECT_EQ(refs[1].offset, 10u);
    EXPECT_EQ(refs[2].offset, 20u);

    refs.clear();
    const auto start = prefix_key_for(schema, 1);
    ASSERT_TRUE(TreeReader::scan_data_blocks_in_range(filesystem_,
                                                      *key_reader,
                                                      schema,
                                                      result->root,
                                                      std::optional<types::PrefixKey>{start},
                                                      std::optional<types::PrefixKey>{limit},
                                                      &refs)
                    .ok());
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].offset, 10u);
    EXPECT_EQ(refs[1].offset, 20u);
}

TEST_F(IndexTreeTest, ForwardCursorKeepsBoundedStateAndTraversesAllBlocks) {
    auto schema = InternalSchema::make(make_schema());
    auto key_file = create_file("key");
    TreeBuilder builder(schema, 2, 4096, 8192, {}, filesystem_, key_file);

    constexpr uint64_t kBlockCount = 64;
    for (uint64_t i = 0; i < kBlockCount; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();
    auto key_reader = filesystem_->open(path("key"));
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();

    auto cursor = ForwardCursor::open(
        filesystem_, *key_reader, schema, result->root, std::nullopt, std::nullopt);
    ASSERT_TRUE(cursor.ok()) << cursor.status();

    std::vector<BlockRef> refs;
    size_t max_slots = 0;
    while (cursor->valid()) {
        refs.push_back(cursor->current());
        max_slots = std::max(max_slots, cursor->state_slots_for_test());
        ASSERT_TRUE(cursor->next().ok());
    }
    ASSERT_TRUE(cursor->status().ok());

    ASSERT_EQ(refs.size(), kBlockCount);
    EXPECT_EQ(refs.front().offset, 0u);
    EXPECT_EQ(refs.back().offset, (kBlockCount - 1) * 10);
    EXPECT_LE(max_slots, 8u);
}

TEST_F(IndexTreeTest, RangeScanPropagatesShortReadFailure) {
    auto schema = InternalSchema::make(make_schema());
    auto key_file = create_file("key");
    TreeBuilder builder(schema, 2, 4096, 8192, {}, filesystem_, key_file);

    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }
    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();

    auto key_reader = filesystem_->open(path("key"));
    ASSERT_TRUE(key_reader.ok()) << key_reader.status();
    auto failing_filesystem = std::make_shared<ReadFailingFileSystem>(filesystem_);

    std::vector<BlockRef> refs;
    auto status =
        TreeReader::scan_data_blocks(failing_filesystem, *key_reader, schema, result->root, &refs);
    EXPECT_FALSE(status.ok());
    EXPECT_TRUE(absl::IsDataLoss(status));
}

} // namespace
} // namespace pl::sstv2::index
