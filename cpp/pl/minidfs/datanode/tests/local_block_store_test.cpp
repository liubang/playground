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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/minidfs/datanode/local_block_store.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

class LocalBlockStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("minidfs_test_" + std::to_string(::getpid()) +
                                                 "_" + std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0; // No reservation for testing
        store_ = std::make_unique<LocalBlockStore>(std::move(config));

        auto init_result = store_->init();
        ASSERT_TRUE(init_result.hasValue()) << "init failed";
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    static inline int counter_ = 0;
};

// init tests
TEST_F(LocalBlockStoreTest, InitCreatesDirectories) {
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp"));
    EXPECT_TRUE(fs::exists(test_dir_ / "current"));
    EXPECT_TRUE(fs::exists(test_dir_ / "trash"));
}

// create_block tests
TEST_F(LocalBlockStoreTest, CreateBlock) {
    auto result = store_->create_block(100, 1, 0, 5000);
    ASSERT_TRUE(result.hasValue());

    // Block should exist in tmp/
    auto path = test_dir_ / "tmp" / "blk_100_5000.blk";
    EXPECT_TRUE(fs::exists(path));
}

TEST_F(LocalBlockStoreTest, CreateBlockDuplicate) {
    auto r1 = store_->create_block(100, 1, 0, 5000);
    ASSERT_TRUE(r1.hasValue());

    auto r2 = store_->create_block(100, 1, 0, 5000);
    EXPECT_TRUE(r2.hasValue());

    auto different_identity = store_->create_block(100, 2, 0, 5000);
    EXPECT_TRUE(different_identity.hasError());
}

// append_chunk tests
TEST_F(LocalBlockStoreTest, AppendChunk) {
    store_->create_block(200, 1, 0, 6000);

    std::string data = "hello world";
    auto result = store_->append_chunk(200, 6000, data.data(), data.size(), 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), data.size());
}

TEST_F(LocalBlockStoreTest, AppendMultipleChunks) {
    store_->create_block(201, 1, 0, 6001);

    std::string chunk1 = "aaaa";
    std::string chunk2 = "bbbb";
    std::string chunk3 = "cccc";

    auto r1 = store_->append_chunk(201, 6001, chunk1.data(), chunk1.size(), 0);
    ASSERT_TRUE(r1.hasValue());
    EXPECT_EQ(r1.value(), 4u);

    auto r2 = store_->append_chunk(201, 6001, chunk2.data(), chunk2.size(), 1);
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(r2.value(), 8u);

    auto r3 = store_->append_chunk(201, 6001, chunk3.data(), chunk3.size(), 2);
    ASSERT_TRUE(r3.hasValue());
    EXPECT_EQ(r3.value(), 12u);
}

TEST_F(LocalBlockStoreTest, AppendChunkToNonexistent) {
    auto result = store_->append_chunk(999, 1, "x", 1, 0);
    EXPECT_TRUE(result.hasError());
}

// finalize_block tests
TEST_F(LocalBlockStoreTest, FinalizeBlock) {
    store_->create_block(300, 1, 0, 7000);
    std::string data = "finalize me";
    store_->append_chunk(300, 7000, data.data(), data.size(), 0);

    auto result = store_->finalize_block(300, 7000);
    ASSERT_TRUE(result.hasValue());

    // Should be in current/ now
    EXPECT_TRUE(fs::exists(test_dir_ / "current" / "blk_300_7000.blk"));
    // Should not be in tmp/
    EXPECT_FALSE(fs::exists(test_dir_ / "tmp" / "blk_300_7000.blk"));
}

TEST_F(LocalBlockStoreTest, FinalizeBlockNotFound) {
    auto result = store_->finalize_block(999, 1);
    EXPECT_TRUE(result.hasError());
}

// delete_block tests
TEST_F(LocalBlockStoreTest, DeleteBlock) {
    store_->create_block(400, 1, 0, 8000);
    store_->finalize_block(400, 8000);

    auto result = store_->delete_block(400, 8000);
    ASSERT_TRUE(result.hasValue());

    // Should be in trash/ now
    EXPECT_TRUE(fs::exists(test_dir_ / "trash" / "blk_400_8000.blk"));
    EXPECT_FALSE(fs::exists(test_dir_ / "current" / "blk_400_8000.blk"));
}

TEST_F(LocalBlockStoreTest, DeleteBlockNotInCurrent) {
    auto result = store_->delete_block(999, 1);
    EXPECT_TRUE(result.hasError());
}

TEST_F(LocalBlockStoreTest, TruncateBlockRebuildsChecksumsAcrossChunks) {
    store_->create_block(450, 1, 0, 8500);
    std::string first = "abcd";
    std::string second = "efgh";
    store_->append_chunk(450, 8500, first.data(), first.size(), 0);
    store_->append_chunk(450, 8500, second.data(), second.size(), 1);
    store_->finalize_block(450, 8500);

    ASSERT_TRUE(store_->truncate_block(450, 8500, 6).hasValue());
    ASSERT_TRUE(store_->truncate_block(450, 8500, 6).hasValue());

    auto data = store_->read_block_data(450, 8500);
    ASSERT_TRUE(data.hasValue());
    EXPECT_EQ(data.value(), "abcdef");
    auto chunk = store_->read_chunk(450, 8500, 1);
    ASSERT_TRUE(chunk.hasValue());
    EXPECT_EQ(chunk.value(), "ef");
    auto verify = store_->verify_block(450, 8500);
    ASSERT_TRUE(verify.hasValue());
    EXPECT_TRUE(verify.value());
}

TEST_F(LocalBlockStoreTest, TruncateBlockRejectsExpansion) {
    store_->create_block(451, 1, 0, 8501);
    store_->append_chunk(451, 8501, "data", 4, 0);
    store_->finalize_block(451, 8501);

    EXPECT_TRUE(store_->truncate_block(451, 8501, 5).hasError());
}

// purge_trash tests
TEST_F(LocalBlockStoreTest, PurgeTrash) {
    store_->create_block(500, 1, 0, 9000);
    store_->finalize_block(500, 9000);
    store_->delete_block(500, 9000);

    store_->create_block(501, 1, 1, 9001);
    store_->finalize_block(501, 9001);
    store_->delete_block(501, 9001);

    auto result = store_->purge_trash();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), 2u);

    // Trash should be empty now
    EXPECT_TRUE(fs::is_empty(test_dir_ / "trash"));
}

TEST_F(LocalBlockStoreTest, PurgeEmptyTrash) {
    auto result = store_->purge_trash();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), 0u);
}

// read_block_data tests
TEST_F(LocalBlockStoreTest, ReadBlockData) {
    store_->create_block(600, 1, 0, 10000);
    std::string data = "read this data";
    store_->append_chunk(600, 10000, data.data(), data.size(), 0);
    store_->finalize_block(600, 10000);

    auto result = store_->read_block_data(600, 10000);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), data);
}

TEST_F(LocalBlockStoreTest, ReadBlockDataMultipleChunks) {
    store_->create_block(601, 1, 0, 10001);
    std::string chunk1 = "AAAA";
    std::string chunk2 = "BBBB";
    store_->append_chunk(601, 10001, chunk1.data(), chunk1.size(), 0);
    store_->append_chunk(601, 10001, chunk2.data(), chunk2.size(), 1);
    store_->finalize_block(601, 10001);

    auto result = store_->read_block_data(601, 10001);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), "AAAABBBB");
}

TEST_F(LocalBlockStoreTest, ReadBlockDataNotFound) {
    auto result = store_->read_block_data(999, 1);
    EXPECT_TRUE(result.hasError());
}

// read_chunk tests
TEST_F(LocalBlockStoreTest, ReadChunk) {
    store_->create_block(700, 1, 0, 11000);
    std::string chunk0 = "first";
    std::string chunk1 = "second";
    store_->append_chunk(700, 11000, chunk0.data(), chunk0.size(), 0);
    store_->append_chunk(700, 11000, chunk1.data(), chunk1.size(), 1);
    store_->finalize_block(700, 11000);

    auto r0 = store_->read_chunk(700, 11000, 0);
    ASSERT_TRUE(r0.hasValue());
    EXPECT_EQ(r0.value(), "first");

    auto r1 = store_->read_chunk(700, 11000, 1);
    ASSERT_TRUE(r1.hasValue());
    EXPECT_EQ(r1.value(), "second");
}

TEST_F(LocalBlockStoreTest, ReadChunkOutOfBounds) {
    store_->create_block(701, 1, 0, 11001);
    std::string data = "one";
    store_->append_chunk(701, 11001, data.data(), data.size(), 0);
    store_->finalize_block(701, 11001);

    auto result = store_->read_chunk(701, 11001, 5);
    EXPECT_TRUE(result.hasError());
}

// verify_block tests
TEST_F(LocalBlockStoreTest, VerifyBlockValid) {
    store_->create_block(800, 1, 0, 12000);
    std::string data = "verify me";
    store_->append_chunk(800, 12000, data.data(), data.size(), 0);
    store_->finalize_block(800, 12000);

    auto result = store_->verify_block(800, 12000);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value());
}

TEST_F(LocalBlockStoreTest, VerifyBlockCorrupt) {
    store_->create_block(801, 1, 0, 12001);
    std::string data = "verify me too";
    store_->append_chunk(801, 12001, data.data(), data.size(), 0);
    store_->finalize_block(801, 12001);

    // Corrupt the data region by overwriting some bytes
    auto path = test_dir_ / "current" / "blk_801_12001.blk";
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    // Seek past header (kBlockHeaderSize) and corrupt data
    f.seekp(static_cast<std::streamoff>(sizeof(BlockHeader) + 2));
    char bad = 'X';
    f.write(&bad, 1);
    f.close();

    auto result = store_->verify_block(801, 12001);
    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value());
}

// report_blocks tests
TEST_F(LocalBlockStoreTest, ReportBlocksEmpty) {
    auto result = store_->report_blocks();
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST_F(LocalBlockStoreTest, ReportBlocksMultiple) {
    store_->create_block(900, 1, 0, 13000);
    std::string d1 = "block one";
    store_->append_chunk(900, 13000, d1.data(), d1.size(), 0);
    store_->finalize_block(900, 13000);

    store_->create_block(901, 1, 1, 13001);
    std::string d2 = "block two!!";
    store_->append_chunk(901, 13001, d2.data(), d2.size(), 0);
    store_->finalize_block(901, 13001);

    auto result = store_->report_blocks();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 2u);

    // Check both blocks are reported
    bool found_900 = false, found_901 = false;
    for (const auto& info : result.value()) {
        if (info.block_id == 900) {
            found_900 = true;
            EXPECT_EQ(info.generation_stamp, 13000u);
            EXPECT_EQ(info.length, d1.size());
        }
        if (info.block_id == 901) {
            found_901 = true;
            EXPECT_EQ(info.generation_stamp, 13001u);
            EXPECT_EQ(info.length, d2.size());
        }
    }
    EXPECT_TRUE(found_900);
    EXPECT_TRUE(found_901);
}

// has_block tests
TEST_F(LocalBlockStoreTest, HasBlock) {
    store_->create_block(1000, 1, 0, 14000);
    store_->finalize_block(1000, 14000);

    EXPECT_TRUE(store_->has_block(1000, 14000));
    EXPECT_FALSE(store_->has_block(1000, 14001)); // wrong gs
    EXPECT_FALSE(store_->has_block(1001, 14000)); // wrong id
}

// available_bytes tests
TEST_F(LocalBlockStoreTest, AvailableBytes) {
    auto result = store_->available_bytes();
    ASSERT_TRUE(result.hasValue());
    // On any real filesystem, available should be > 0 (with reserved_bytes=0)
    EXPECT_GT(result.value(), 0u);
}

// Full lifecycle test
TEST_F(LocalBlockStoreTest, FullLifecycle) {
    // Create -> append -> finalize -> read -> verify -> delete -> purge
    auto create = store_->create_block(1100, 42, 0, 15000);
    ASSERT_TRUE(create.hasValue());

    std::string data = "lifecycle test data payload";
    auto append = store_->append_chunk(1100, 15000, data.data(), data.size(), 0);
    ASSERT_TRUE(append.hasValue());

    auto finalize = store_->finalize_block(1100, 15000);
    ASSERT_TRUE(finalize.hasValue());

    EXPECT_TRUE(store_->has_block(1100, 15000));

    auto read = store_->read_block_data(1100, 15000);
    ASSERT_TRUE(read.hasValue());
    EXPECT_EQ(read.value(), data);

    auto verify = store_->verify_block(1100, 15000);
    ASSERT_TRUE(verify.hasValue());
    EXPECT_TRUE(verify.value());

    auto del = store_->delete_block(1100, 15000);
    ASSERT_TRUE(del.hasValue());
    EXPECT_FALSE(store_->has_block(1100, 15000));

    auto purge = store_->purge_trash();
    ASSERT_TRUE(purge.hasValue());
    EXPECT_EQ(purge.value(), 1u);
}

// cleanup_stale_tmp_blocks tests
TEST_F(LocalBlockStoreTest, CleanupStaleTmpBlocks) {
    // Create a block in tmp/ but do NOT finalize it
    store_->create_block(1200, 1, 0, 16000);
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_1200_16000.blk"));

    // Create a second store with a very short stale threshold
    LocalBlockStore::Config cfg;
    cfg.storage_root = test_dir_.string();
    cfg.reserved_bytes = 0;
    cfg.tmp_cleanup_stale_after_ms = 0; // 0 means clean up all tmp blocks
    LocalBlockStore stale_store(std::move(cfg));
    auto init_result = stale_store.init();
    ASSERT_TRUE(init_result.hasValue()) << "init failed";

    // The stale tmp block should have been cleaned up during init
    EXPECT_FALSE(fs::exists(test_dir_ / "tmp" / "blk_1200_16000.blk"));
}

TEST_F(LocalBlockStoreTest, CleanupRespectsExclusion) {
    store_->create_block(1201, 1, 0, 16001);
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_1201_16001.blk"));

    LocalBlockStore::Config cfg;
    cfg.storage_root = test_dir_.string();
    cfg.reserved_bytes = 0;
    cfg.tmp_cleanup_stale_after_ms = 0; // 0 means clean up all tmp blocks
    LocalBlockStore stale_store(std::move(cfg));
    auto init_result = stale_store.init(/*active_tmp_block=*/std::make_pair(1201ULL, 16001ULL));
    ASSERT_TRUE(init_result.hasValue());

    // The excluded block should NOT have been removed
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_1201_16001.blk"));
}

TEST_F(LocalBlockStoreTest, CleanupSkipsFreshBlocks) {
    // Create a store with a very long stale threshold
    LocalBlockStore::Config cfg;
    cfg.storage_root = test_dir_.string();
    cfg.reserved_bytes = 0;
    cfg.tmp_cleanup_stale_after_ms = 3600000; // 1 hour — nothing is stale
    LocalBlockStore fresh_store(std::move(cfg));
    auto init_result = fresh_store.init();
    ASSERT_TRUE(init_result.hasValue());

    fresh_store.create_block(1202, 1, 0, 16002);
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_1202_16002.blk"));

    // Re-init: the fresh block should still be there
    auto reinit = fresh_store.init();
    ASSERT_TRUE(reinit.hasValue());
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_1202_16002.blk"));
}

} // namespace
} // namespace pl::minidfs
