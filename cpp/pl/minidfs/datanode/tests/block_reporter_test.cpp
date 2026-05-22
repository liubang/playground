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

#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/datanode/block_reporter.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

class BlockReporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ =
            fs::temp_directory_path() / ("minidfs_reporter_test_" + std::to_string(::getpid()) +
                                         "_" + std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0;
        store_ = std::make_unique<LocalBlockStore>(std::move(config));
        store_->init();
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    /// Helper: create and finalize a block with some data.
    void create_finalized_block(uint64_t block_id, uint64_t gs) {
        store_->create_block(block_id, 1, 0, gs);
        std::string data = "data_" + std::to_string(block_id);
        store_->append_chunk(block_id, gs, data.data(), data.size());
        store_->finalize_block(block_id, gs);
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    static inline int counter_ = 0;
};

TEST_F(BlockReporterTest, SendFullReportEmpty) {
    std::atomic<int> report_count{0};
    BlockReportFunc report_func =
        [&](uint64_t datanode_id,
            const std::vector<BlockInfo>& blocks) -> Result<BlockReportResponse> {
        report_count++;
        EXPECT_EQ(datanode_id, 42u);
        EXPECT_TRUE(blocks.empty());
        return BlockReportResponse{};
    };

    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 42;
    cfg.full_report_interval_ms = 100000; // won't trigger in this test

    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);
    auto result = reporter.send_full_report();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(report_count.load(), 1);
}

TEST_F(BlockReporterTest, SendFullReportWithBlocks) {
    create_finalized_block(100, 5000);
    create_finalized_block(101, 5001);

    std::vector<BlockInfo> reported_blocks;
    BlockReportFunc report_func =
        [&](uint64_t datanode_id,
            const std::vector<BlockInfo>& blocks) -> Result<BlockReportResponse> {
        reported_blocks = blocks;
        return BlockReportResponse{};
    };
    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);

    auto result = reporter.send_full_report();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(reported_blocks.size(), 2u);
}

TEST_F(BlockReporterTest, ReportResponseDeletesBlocks) {
    create_finalized_block(200, 6000);
    create_finalized_block(201, 6001);

    std::vector<std::pair<uint64_t, uint64_t>> deleted;
    BlockReportFunc report_func = [](uint64_t,
                                     const std::vector<BlockInfo>&) -> Result<BlockReportResponse> {
        BlockReportResponse resp;
        resp.blocks_to_delete = {200}; // NN says delete block 200
        return resp;
    };
    DeleteBlockFunc delete_func = [&](uint64_t block_id, uint64_t gs) {
        deleted.emplace_back(block_id, gs);
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);

    auto result = reporter.send_full_report();
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(deleted.size(), 1u);
    EXPECT_EQ(deleted[0].first, 200u);
}

TEST_F(BlockReporterTest, NotifyBlockFinalized) {
    BlockReportFunc report_func = [](uint64_t,
                                     const std::vector<BlockInfo>&) -> Result<BlockReportResponse> {
        return BlockReportResponse{};
    };
    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);

    // Should not crash, just update internal tracking
    reporter.notify_block_finalized(300, 7000);
    reporter.notify_block_finalized(301, 7001);
}

TEST_F(BlockReporterTest, NotifyBlockDeleted) {
    BlockReportFunc report_func = [](uint64_t,
                                     const std::vector<BlockInfo>&) -> Result<BlockReportResponse> {
        return BlockReportResponse{};
    };
    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);

    reporter.notify_block_finalized(400, 8000);
    reporter.notify_block_deleted(400);
    // The block should now be in the removed set, not added set
}

TEST_F(BlockReporterTest, StartAndStop) {
    std::atomic<int> report_count{0};
    BlockReportFunc report_func =
        [&](uint64_t, const std::vector<BlockInfo>&) -> Result<BlockReportResponse> {
        report_count++;
        return BlockReportResponse{};
    };
    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    cfg.full_report_interval_ms = 50; // short interval for test

    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);
    EXPECT_FALSE(reporter.running());

    reporter.start();
    EXPECT_TRUE(reporter.running());

    // Wait enough time for at least one report
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    reporter.stop();
    EXPECT_FALSE(reporter.running());
    EXPECT_GE(report_count.load(), 1);
}

TEST_F(BlockReporterTest, ReportRPCFailure) {
    BlockReportFunc report_func = [](uint64_t,
                                     const std::vector<BlockInfo>&) -> Result<BlockReportResponse> {
        return pl::makeError(pl::Status(5000, "RPC failed"));
    };
    DeleteBlockFunc delete_func = [](uint64_t, uint64_t) {
    };

    BlockReporter::Config cfg;
    cfg.datanode_id = 1;
    BlockReporter reporter(cfg, store_.get(), report_func, delete_func);

    auto result = reporter.send_full_report();
    EXPECT_TRUE(result.hasError());
}

} // namespace
} // namespace pl::minidfs
