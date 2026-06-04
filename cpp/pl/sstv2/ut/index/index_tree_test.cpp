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
// Created: 2026/06/04 15:10

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/index/index_block_writer.h"
#include "cpp/pl/sstv2/index/index_iterator.h"
#include "cpp/pl/sstv2/index/index_tree_builder.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::index;

// === IndexBlockWriter Tests ===

TEST(IndexBlockWriterTest, EmptyBlock) {
    IndexBlockWriter writer;
    EXPECT_TRUE(writer.empty());
    EXPECT_EQ(writer.count(), 0u);
}

TEST(IndexBlockWriterTest, AddEntries) {
    IndexBlockWriter writer;
    writer.add_entry("aaa", 0, 100);
    writer.add_entry("bbb", 100, 200);
    writer.add_entry("ccc", 300, 150);

    EXPECT_FALSE(writer.empty());
    EXPECT_EQ(writer.count(), 3u);
    EXPECT_EQ(writer.last_key(), "ccc");
    EXPECT_GT(writer.estimated_size(), 0u);
}

TEST(IndexBlockWriterTest, FinishProducesNonEmpty) {
    IndexBlockWriter writer;
    writer.add_entry("key1", 0, 1024);
    writer.add_entry("key2", 1024, 2048);

    std::string block = writer.finish();
    EXPECT_GT(block.size(), 8u); // magic(4) + at least some data + checksum(4)
}

TEST(IndexBlockWriterTest, Reset) {
    IndexBlockWriter writer;
    writer.add_entry("key1", 0, 100);
    writer.add_entry("key2", 100, 200);
    writer.reset();

    EXPECT_TRUE(writer.empty());
    EXPECT_EQ(writer.count(), 0u);
}

// === IndexTreeBuilder Tests ===

TEST(IndexTreeBuilderTest, SingleDataBlock) {
    IndexTreeBuilder builder;
    builder.add_data_block("last_key_0", 0, 4096);

    auto result = builder.finish();
    EXPECT_GE(result.tree_height, 1u);
    EXPECT_FALSE(result.index_blocks.empty());
}

TEST(IndexTreeBuilderTest, MultipleDataBlocks) {
    IndexTreeBuilder builder;

    // Simulate 10 data blocks
    for (int i = 0; i < 10; ++i) {
        std::string key =
            "key_" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i);
        builder.add_data_block(key, i * 4096, 4096);
    }

    auto result = builder.finish();
    EXPECT_GE(result.tree_height, 1u);
    EXPECT_FALSE(result.index_blocks.empty());
}

TEST(IndexTreeBuilderTest, EmptyTree) {
    IndexTreeBuilder builder;
    auto result = builder.finish();
    EXPECT_EQ(result.tree_height, 0u);
    EXPECT_TRUE(result.index_blocks.empty());
}

TEST(IndexTreeBuilderTest, SmallBlockSizeForcesMultiLevel) {
    // Use a very small index_block_size to force multiple levels
    IndexTreeOptions opts;
    opts.index_block_size = 64; // Very small, will force splits
    IndexTreeBuilder builder(opts);

    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i + 1000);
        builder.add_data_block(key, i * 4096, 4096);
    }

    auto result = builder.finish();
    EXPECT_GE(result.tree_height, 2u); // Should be multi-level
    EXPECT_GT(result.index_blocks.size(), 1u);
}

// === IndexIterator Tests (end-to-end with builder) ===

class IndexIteratorTest : public ::testing::Test {
protected:
    // Build an index tree and assemble a fake "file" containing only the index
    // blocks (data blocks are represented only by their offset/size metadata).
    void build_index(int num_data_blocks, size_t index_block_size = 4096) {
        IndexTreeOptions opts;
        opts.index_block_size = index_block_size;
        // Data blocks occupy [0, num_data_blocks * block_size).
        // Index region starts right after.
        uint64_t data_region_size = num_data_blocks * data_block_size_;
        opts.index_region_offset = data_region_size;

        IndexTreeBuilder builder(opts);
        for (int i = 0; i < num_data_blocks; ++i) {
            std::string key = make_key(i);
            builder.add_data_block(key, i * data_block_size_, data_block_size_);
        }

        auto result = builder.finish();
        tree_height_ = result.tree_height;
        root_offset_ = result.root_offset;

        // Build a contiguous buffer: [padding for data region][index blocks...]
        file_data_.resize(data_region_size);
        for (const auto& block : result.index_blocks) {
            file_data_.append(block);
        }
    }

    static std::string make_key(int i) {
        // Zero-padded to ensure lexicographic ordering matches numeric ordering
        char buf[32];
        snprintf(buf, sizeof(buf), "key_%06d", i);
        return std::string(buf);
    }

    std::span<const std::byte> file_span() const {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(file_data_.data()),
                                          file_data_.size());
    }

    static constexpr uint32_t data_block_size_ = 4096;
    std::string file_data_;
    uint64_t root_offset_ = 0;
    uint8_t tree_height_ = 0;
};

TEST_F(IndexIteratorTest, SeekFirst) {
    build_index(10);
    ASSERT_GT(tree_height_, 0);

    auto iter_or = IndexIterator::open(file_span(), root_offset_, tree_height_);
    ASSERT_TRUE(iter_or.ok()) << iter_or.status();
    auto iter = std::move(*iter_or);

    // Seek to the very beginning
    ASSERT_TRUE(iter.seek("").ok());
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ(iter.current_block_offset(), 0u);
    EXPECT_EQ(iter.current_block_size(), data_block_size_);
}

TEST_F(IndexIteratorTest, SeekExactKey) {
    build_index(10);

    auto iter = std::move(*IndexIterator::open(file_span(), root_offset_, tree_height_));

    // Seek to key_000005 — should land on the block whose last_key >= key_000005
    ASSERT_TRUE(iter.seek(make_key(5)).ok());
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ(iter.current_block_offset(), 5u * data_block_size_);
}

TEST_F(IndexIteratorTest, SeekPastEnd) {
    build_index(10);

    auto iter = std::move(*IndexIterator::open(file_span(), root_offset_, tree_height_));

    // Seek past the last key
    ASSERT_TRUE(iter.seek("zzz_past_everything").ok());
    // Iterator should be invalid (no block has last_key >= target)
    EXPECT_FALSE(iter.valid());
}

TEST_F(IndexIteratorTest, SequentialScan) {
    build_index(10);

    auto iter = std::move(*IndexIterator::open(file_span(), root_offset_, tree_height_));

    ASSERT_TRUE(iter.seek("").ok());

    // Scan all data blocks in order
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(iter.valid()) << "Expected valid at block " << i;
        EXPECT_EQ(iter.current_block_offset(), static_cast<uint64_t>(i) * data_block_size_);
        EXPECT_EQ(iter.current_block_size(), data_block_size_);
        if (i < 9) {
            ASSERT_TRUE(iter.next().ok());
        }
    }
    // After last block, next should make it invalid
    ASSERT_TRUE(iter.next().ok());
    EXPECT_FALSE(iter.valid());
}

TEST_F(IndexIteratorTest, MultiLevelTree) {
    // Small index_block_size to force multi-level
    build_index(100, 128);
    ASSERT_GE(tree_height_, 2u);

    auto iter = std::move(*IndexIterator::open(file_span(), root_offset_, tree_height_));

    // Seek to middle
    ASSERT_TRUE(iter.seek(make_key(50)).ok());
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ(iter.current_block_offset(), 50u * data_block_size_);

    // Continue scanning
    ASSERT_TRUE(iter.next().ok());
    ASSERT_TRUE(iter.valid());
    EXPECT_EQ(iter.current_block_offset(), 51u * data_block_size_);
}

TEST_F(IndexIteratorTest, InvalidRootOffset) {
    build_index(5);

    auto result = IndexIterator::open(file_span(), file_data_.size() + 100, tree_height_);
    EXPECT_FALSE(result.ok());
}

TEST_F(IndexIteratorTest, ZeroTreeHeight) {
    build_index(5);

    auto result = IndexIterator::open(file_span(), root_offset_, 0);
    EXPECT_FALSE(result.ok());
}
