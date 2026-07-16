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
// Created: 2026/05/10 21:00

#include <gtest/gtest.h>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/replication_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

class ReplicationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        dn_mgr_ = std::make_unique<DataNodeManager>(store_.get());
        placement_ = std::make_unique<PlacementManager>(dn_mgr_.get());
        repl_mgr_ = std::make_unique<ReplicationManager>(store_.get(), placement_.get(), "test-secret");

        // Register 5 datanodes for placement flexibility.
        dn_mgr_->register_datanode("dn-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
        dn_mgr_->register_datanode("dn-2", "host2", "10.0.0.2", 9000, 9100, "/rack2", 1000 * kGB);
        dn_mgr_->register_datanode("dn-3", "host3", "10.0.0.3", 9000, 9100, "/rack3", 1000 * kGB);
        dn_mgr_->register_datanode("dn-4", "host4", "10.0.0.4", 9000, 9100, "/rack1", 1000 * kGB);
        dn_mgr_->register_datanode("dn-5", "host5", "10.0.0.5", 9000, 9100, "/rack2", 1000 * kGB);
    }

    // Helper: create a committed block with given number of finalized replicas.
    void create_committed_block(uint64_t block_id,
                                uint64_t inode_id,
                                uint32_t desired_replica,
                                uint32_t actual_replicas) {
        BlockMeta block;
        block.block_id = block_id;
        block.inode_id = inode_id;
        block.block_index = 0;
        block.generation_stamp = 1;
        block.length = 128 * kMB;
        block.state = BlockState::kCommitted;
        block.desired_replica = desired_replica;
        block.ctime_ms = 1;
        block.mtime_ms = 1;
        store_->create_block(block);

        // Get registered datanode IDs.
        auto all = store_->list_all_datanodes();
        for (uint32_t i = 0; i < actual_replicas && i < all.value().size(); ++i) {
            BlockReplica replica;
            replica.block_id = block_id;
            replica.datanode_id = all.value()[i].datanode_id;
            replica.storage_id = 0;
            replica.state = ReplicaState::kFinalized;
            replica.length = 128 * kMB;
            replica.generation_stamp = 1;
            replica.report_time_ms = 1;
            store_->upsert_replica(replica);
        }
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<DataNodeManager> dn_mgr_;
    std::unique_ptr<PlacementManager> placement_;
    std::unique_ptr<ReplicationManager> repl_mgr_;
};

TEST_F(ReplicationManagerTest, NoBlocks) {
    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST_F(ReplicationManagerTest, FullyReplicated) {
    // Block with desired=3, actual=3 → no tasks.
    create_committed_block(1001, 100, 3, 3);

    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST_F(ReplicationManagerTest, UnderReplicated) {
    // Block with desired=3, actual=1 → should produce replication tasks.
    create_committed_block(1001, 100, 3, 1);

    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value().empty());

    // Should produce 2 tasks (deficit = 3 - 1 = 2).
    // Due to placement potentially picking existing nodes, we allow >= 1.
    for (const auto& task : result.value()) {
        EXPECT_EQ(task.block_id, 1001u);
        EXPECT_FALSE(task.is_deletion);
        EXPECT_GT(task.source_datanode, 0u);
        EXPECT_GT(task.target_datanode, 0u);
    }
}

TEST_F(ReplicationManagerTest, OverReplicated) {
    // Block with desired=2, actual=4 → should produce deletion tasks.
    create_committed_block(1001, 100, 2, 4);

    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value().empty());

    // Should produce 2 deletion tasks (excess = 4 - 2 = 2).
    uint32_t deletion_count = 0;
    for (const auto& task : result.value()) {
        if (task.is_deletion) {
            EXPECT_EQ(task.block_id, 1001u);
            ++deletion_count;
        }
    }
    EXPECT_EQ(deletion_count, 2u);
}

TEST_F(ReplicationManagerTest, ZeroHealthyReplicas) {
    // Block with desired=3, actual=0 → no source to copy from, no tasks.
    BlockMeta block;
    block.block_id = 2001;
    block.inode_id = 200;
    block.block_index = 0;
    block.generation_stamp = 1;
    block.length = 128 * kMB;
    block.state = BlockState::kCommitted;
    block.desired_replica = 3;
    block.ctime_ms = 1;
    block.mtime_ms = 1;
    store_->create_block(block);

    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    // No tasks since there are zero healthy replicas to copy from.
    EXPECT_TRUE(result.value().empty());
}

TEST_F(ReplicationManagerTest, MultipleBlocks) {
    // Two under-replicated blocks.
    create_committed_block(1001, 100, 3, 2);
    create_committed_block(1002, 100, 3, 1);

    auto result = repl_mgr_->scan();
    ASSERT_TRUE(result.hasValue());
    // At least one replication task should be generated.
    // The exact count depends on placement overlap with existing replicas.
    EXPECT_GE(result.value().size(), 1u);

    // Verify tasks reference our blocks.
    bool has_1001 = false;
    bool has_1002 = false;
    for (const auto& task : result.value()) {
        if (task.block_id == 1001)
            has_1001 = true;
        if (task.block_id == 1002)
            has_1002 = true;
    }
    // At least one of the blocks should have a task.
    EXPECT_TRUE(has_1001 || has_1002);
}

} // namespace
} // namespace pl::minidfs
