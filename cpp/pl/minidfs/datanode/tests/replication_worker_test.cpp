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
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpp/pl/minidfs/datanode/replication_worker.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

class ReplicationWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ =
            fs::temp_directory_path() / ("minidfs_repworker_test_" + std::to_string(::getpid()) +
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
    void create_finalized_block(uint64_t block_id, uint64_t gs, const std::string& data) {
        store_->create_block(block_id, 1, 0, gs);
        store_->append_chunk(block_id, gs, data.data(), data.size(), 0);
        store_->finalize_block(block_id, gs);
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    static inline int counter_ = 0;
};

TEST_F(ReplicationWorkerTest, CopyTaskSuccess) {
    create_finalized_block(100, 5000, "copy this data");

    struct CopyRecord {
        uint64_t block_id;
        uint64_t gs;
        std::string data;
        std::string host;
        uint32_t port;
    };
    std::mutex mu;
    std::vector<CopyRecord> copies;

    CopyFunc copy_func = [&](uint64_t block_id,
                             uint64_t gs,
                             uint64_t,
                             uint32_t,
                             const std::string& data,
                             const std::string& host,
                             uint32_t port) -> Result<Void> {
        std::lock_guard lock(mu);
        copies.push_back({block_id, gs, data, host, port});
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 1;
    ReplicationWorker worker(cfg, store_.get(), copy_func);
    worker.start();

    worker.enqueue(DataNodeTask{
        .kind = TaskKind::kCopy,
        .block_id = 100,
        .generation_stamp = 5000,
        .target_host = "dn2",
        .target_port = 9001,
    });

    // Wait for task to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    worker.stop();

    std::lock_guard lock(mu);
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].block_id, 100u);
    EXPECT_EQ(copies[0].gs, 5000u);
    EXPECT_EQ(copies[0].data, "copy this data");
    EXPECT_EQ(copies[0].host, "dn2");
    EXPECT_EQ(copies[0].port, 9001u);
}

TEST_F(ReplicationWorkerTest, DeleteTaskSuccess) {
    create_finalized_block(200, 6000, "delete me");
    EXPECT_TRUE(store_->has_block(200, 6000));

    CopyFunc copy_func =
        [](uint64_t, uint64_t, uint64_t, uint32_t, const std::string&, const std::string&, uint32_t)
        -> Result<Void> {
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 1;
    ReplicationWorker worker(cfg, store_.get(), copy_func);
    worker.start();

    worker.enqueue(DataNodeTask{
        .kind = TaskKind::kDelete,
        .block_id = 200,
        .generation_stamp = 6000,
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    worker.stop();

    // Block should be moved to trash
    EXPECT_FALSE(store_->has_block(200, 6000));
    EXPECT_TRUE(fs::exists(test_dir_ / "trash" / "blk_200_6000.blk"));
}

TEST_F(ReplicationWorkerTest, MultipleTasks) {
    create_finalized_block(300, 7000, "data_300");
    create_finalized_block(301, 7001, "data_301");

    std::atomic<int> copy_count{0};
    CopyFunc copy_func = [&](uint64_t,
                             uint64_t,
                             uint64_t,
                             uint32_t,
                             const std::string&,
                             const std::string&,
                             uint32_t) -> Result<Void> {
        copy_count++;
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 2;
    ReplicationWorker worker(cfg, store_.get(), copy_func);
    worker.start();

    worker.enqueue(DataNodeTask{
        .kind = TaskKind::kCopy,
        .block_id = 300,
        .generation_stamp = 7000,
        .target_host = "dn2",
        .target_port = 9001,
    });
    worker.enqueue(DataNodeTask{
        .kind = TaskKind::kCopy,
        .block_id = 301,
        .generation_stamp = 7001,
        .target_host = "dn3",
        .target_port = 9002,
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    worker.stop();

    EXPECT_EQ(copy_count.load(), 2);
}

TEST_F(ReplicationWorkerTest, CopyBlockNotFound) {
    // Block 999 doesn't exist — copy should fail gracefully (not crash)
    std::atomic<int> copy_count{0};
    CopyFunc copy_func = [&](uint64_t,
                             uint64_t,
                             uint64_t,
                             uint32_t,
                             const std::string&,
                             const std::string&,
                             uint32_t) -> Result<Void> {
        copy_count++;
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 1;
    ReplicationWorker worker(cfg, store_.get(), copy_func);
    worker.start();

    worker.enqueue(DataNodeTask{
        .kind = TaskKind::kCopy,
        .block_id = 999,
        .generation_stamp = 1,
        .target_host = "dn2",
        .target_port = 9001,
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    worker.stop();

    // copy_func should NOT have been called since read_block_data failed
    EXPECT_EQ(copy_count.load(), 0);
}

TEST_F(ReplicationWorkerTest, PendingCount) {
    CopyFunc copy_func =
        [](uint64_t, uint64_t, uint64_t, uint32_t, const std::string&, const std::string&, uint32_t)
        -> Result<Void> {
        // Slow task
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 1;
    ReplicationWorker worker(cfg, store_.get(), copy_func);

    // Enqueue without starting — tasks accumulate
    create_finalized_block(400, 8000, "data");
    worker.enqueue(DataNodeTask{.kind = TaskKind::kCopy,
                                .block_id = 400,
                                .generation_stamp = 8000,
                                .target_host = "x",
                                .target_port = 1});
    worker.enqueue(DataNodeTask{.kind = TaskKind::kCopy,
                                .block_id = 400,
                                .generation_stamp = 8000,
                                .target_host = "y",
                                .target_port = 2});

    EXPECT_EQ(worker.pending_count(), 2u);

    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    worker.stop();

    EXPECT_EQ(worker.pending_count(), 0u);
}

TEST_F(ReplicationWorkerTest, StopDrainsQueue) {
    create_finalized_block(500, 9000, "drain_test");

    std::atomic<int> copy_count{0};
    CopyFunc copy_func = [&](uint64_t,
                             uint64_t,
                             uint64_t,
                             uint32_t,
                             const std::string&,
                             const std::string&,
                             uint32_t) -> Result<Void> {
        copy_count++;
        RETURN_VOID;
    };

    ReplicationWorker::Config cfg;
    cfg.max_concurrent_tasks = 2;
    ReplicationWorker worker(cfg, store_.get(), copy_func);
    worker.start();

    // Enqueue a bunch of tasks
    for (int i = 0; i < 5; ++i) {
        worker.enqueue(DataNodeTask{
            .kind = TaskKind::kCopy,
            .block_id = 500,
            .generation_stamp = 9000,
            .target_host = "dn" + std::to_string(i),
            .target_port = static_cast<uint32_t>(9000 + i),
        });
    }

    // Stop waits for active tasks but may not process all queued tasks
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    worker.stop();

    // At least some tasks should have been processed
    EXPECT_GE(copy_count.load(), 1);
}

} // namespace
} // namespace pl::minidfs
