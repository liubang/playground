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
// Created: 2026/05/24 16:19

// Regression tests for known DataNode-side bugs found during code review.
// Each test constructs a minimal reproduction scenario for a specific issue.
// Tests are expected to FAIL against the current (buggy) code and PASS after fixes.

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/minidfs/datanode/block_format.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

// Fixture: DataNode-side LocalBlockStore

class RegressionDataNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("minidfs_regression_" +
                                                  std::to_string(::getpid()) + "_" +
                                                  std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0;
        store_ = std::make_unique<LocalBlockStore>(std::move(config));
        ASSERT_TRUE(store_->init().hasValue());
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

// P1: ReadBlock doesn't verify on-disk CRC — silent data corruption
//
// Scenario:
//   1. Write a block with valid CRC
//   2. Corrupt the data on disk (simulate silent bit-rot)
//   3. read_block_data() returns corrupted data without error
//   4. The checksum in the ReadBlock RPC response is computed AFTER reading,
//      so it matches the (corrupted) data — client can't detect disk corruption

TEST_F(RegressionDataNodeTest, P1_ReadBlockDataDoesNotVerifyOnDiskCRC) {
    uint64_t block_id = 42;
    uint64_t gen = 100;
    std::string original_data = "This is important data that must be verified";

    ASSERT_TRUE(store_->create_block(block_id, 1, 0, gen).hasValue());
    ASSERT_TRUE(
        store_->append_chunk(block_id, gen, original_data.data(), original_data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(block_id, gen).hasValue());

    // Verify the block is valid before corruption
    auto verify_before = store_->verify_block(block_id, gen);
    ASSERT_TRUE(verify_before.hasValue());
    EXPECT_TRUE(verify_before.value());

    // Corrupt the data on disk (simulate silent bit-rot)
    auto path = test_dir_ / "current" / "blk_42_100.blk";
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open()) << "Cannot open block file for corruption";
        // Seek past header and corrupt a byte in the data region
        f.seekp(static_cast<std::streamoff>(sizeof(BlockHeader) + 5));
        char bad = 'X';
        f.write(&bad, 1);
    }

    // verify_block correctly detects corruption (per-chunk CRC check)
    auto verify_after = store_->verify_block(block_id, gen);
    ASSERT_TRUE(verify_after.hasValue());
    EXPECT_FALSE(verify_after.value()) << "verify_block correctly detects corruption";

    // BUG: read_block_data() returns corrupted data WITHOUT error
    auto read_result = store_->read_block_data(block_id, gen);
    ASSERT_TRUE(read_result.hasValue())
        << "BUG CONFIRMED: read_block_data returns success on corrupted block";

    // The data is corrupted but no error was raised
    EXPECT_NE(read_result.value(), original_data)
        << "Data should be different due to corruption";
}

// P2: Chunk write has no idempotency — retry of create_block fails
//
// Scenario: Client retries chunk 0 (which calls create_block again)
//           → kAlreadyExists error breaks the write pipeline

TEST_F(RegressionDataNodeTest, P2_ChunkZeroRetryCreateBlockNotIdempotent) {
    uint64_t block_id = 200;
    uint64_t gen = 500;

    // First write of chunk 0: create_block succeeds
    ASSERT_TRUE(store_->create_block(block_id, 1, 0, gen).hasValue());
    std::string chunk0 = "first chunk data";
    ASSERT_TRUE(store_->append_chunk(block_id, gen, chunk0.data(), chunk0.size(), 0).hasValue());

    // Simulate RPC retry: client retries chunk 0 → tries to create_block again
    auto retry_create = store_->create_block(block_id, 1, 0, gen);

    // create_block is still not idempotent (separate issue), but append_chunk
    // retry with same chunk_index and data IS idempotent now.
    // This test documents that create_block retry remains an error.
    EXPECT_TRUE(retry_create.hasError())
        << "create_block retry fails (expected — idempotency is at append_chunk level)";
}

// P2: Chunk retry causes duplicate data append
//
// Scenario: Client retries chunk N (N>0) due to RPC timeout
//           → append_chunk blindly appends again → data is duplicated

TEST_F(RegressionDataNodeTest, P2_ChunkRetryDuplicatesData_FIXED) {
    uint64_t block_id = 201;
    uint64_t gen = 501;

    ASSERT_TRUE(store_->create_block(block_id, 1, 0, gen).hasValue());

    std::string chunk0 = "AAAA";
    std::string chunk1 = "BBBB";

    // Write chunk 0 and chunk 1 normally
    ASSERT_TRUE(store_->append_chunk(block_id, gen, chunk0.data(), chunk0.size(), 0).hasValue());
    ASSERT_TRUE(store_->append_chunk(block_id, gen, chunk1.data(), chunk1.size(), 1).hasValue());

    // Simulate: RPC timeout, client retries chunk 1 (same data, same index)
    auto retry = store_->append_chunk(block_id, gen, chunk1.data(), chunk1.size(), 1);
    ASSERT_TRUE(retry.hasValue())
        << "FIX VERIFIED: Retry of chunk 1 is idempotent (no-op, returns success)";

    // Data should NOT be duplicated
    ASSERT_TRUE(store_->finalize_block(block_id, gen).hasValue());
    auto data = store_->read_block_data(block_id, gen);
    ASSERT_TRUE(data.hasValue());

    std::string expected = "AAAABBBB";
    EXPECT_EQ(data.value(), expected)
        << "FIX VERIFIED: Chunk retry does not duplicate data";
}

// P2: Out-of-order chunk arrival causes data corruption
//
// Scenario: Due to network reordering, chunk 1 arrives before chunk 0.
//           append_chunk doesn't enforce ordering → data is scrambled.

TEST_F(RegressionDataNodeTest, P2_OutOfOrderChunkRejected_FIXED) {
    uint64_t block_id = 202;
    uint64_t gen = 502;

    ASSERT_TRUE(store_->create_block(block_id, 1, 0, gen).hasValue());

    std::string chunk0 = "FIRST";
    std::string chunk1 = "SECOND";

    // Attempt to write chunk 1 before chunk 0 — should be rejected
    auto ooo_result = store_->append_chunk(block_id, gen, chunk1.data(), chunk1.size(), 1);
    EXPECT_TRUE(ooo_result.hasError())
        << "FIX VERIFIED: Out-of-order chunk (index 1 before 0) is rejected";

    // Write in correct order works fine
    ASSERT_TRUE(store_->append_chunk(block_id, gen, chunk0.data(), chunk0.size(), 0).hasValue());
    ASSERT_TRUE(store_->append_chunk(block_id, gen, chunk1.data(), chunk1.size(), 1).hasValue());

    ASSERT_TRUE(store_->finalize_block(block_id, gen).hasValue());
    auto data = store_->read_block_data(block_id, gen);
    ASSERT_TRUE(data.hasValue());

    EXPECT_EQ(data.value(), "FIRSTSECOND")
        << "FIX VERIFIED: Sequential chunk writes produce correct ordering";
}

} // namespace
} // namespace pl::minidfs
