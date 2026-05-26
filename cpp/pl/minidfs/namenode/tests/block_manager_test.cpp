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
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

std::vector<uint64_t> datanode_ids(const LocatedBlock& block) {
    std::vector<uint64_t> ids;
    ids.reserve(block.locations.size());
    for (const auto& location : block.locations) {
        ids.push_back(location.datanode_id);
    }
    return ids;
}

class BlockManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        dn_mgr_ = std::make_unique<DataNodeManager>(store_.get());
        placement_ = std::make_unique<PlacementManager>(dn_mgr_.get());
        block_mgr_ = std::make_unique<BlockManager>(store_.get(), placement_.get());

        // Register 3 datanodes so placement can succeed.
        dn_mgr_->register_datanode("dn-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
        dn_mgr_->register_datanode("dn-2", "host2", "10.0.0.2", 9000, 9100, "/rack2", 1000 * kGB);
        dn_mgr_->register_datanode("dn-3", "host3", "10.0.0.3", 9000, 9100, "/rack3", 1000 * kGB);
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<DataNodeManager> dn_mgr_;
    std::unique_ptr<PlacementManager> placement_;
    std::unique_ptr<BlockManager> block_mgr_;
};

TEST_F(BlockManagerTest, AllocateBlock) {
    auto result =
        block_mgr_->allocate_block(/*inode_id=*/100, /*block_index=*/0, /*replication=*/3);
    ASSERT_TRUE(result.hasValue());

    auto& lb = result.value();
    EXPECT_GT(lb.block_id, 0u);
    EXPECT_GT(lb.generation_stamp, 0u);
    EXPECT_EQ(lb.locations.size(), 3u);

    // Verify the block was persisted in the store.
    auto block = store_->get_block(lb.block_id);
    ASSERT_TRUE(block.hasValue());
    EXPECT_EQ(block.value().state, BlockState::kAllocating);
    EXPECT_EQ(block.value().inode_id, 100u);
}

TEST_F(BlockManagerTest, CommitBlock) {
    auto alloc = block_mgr_->allocate_block(/*inode_id=*/100, /*block_index=*/0, /*replication=*/3);
    ASSERT_TRUE(alloc.hasValue());

    auto result = block_mgr_->commit_block(alloc.value().block_id,
                                           128 * kMB,
                                           alloc.value().generation_stamp,
                                           datanode_ids(alloc.value()));
    ASSERT_TRUE(result.hasValue());

    auto block = store_->get_block(alloc.value().block_id);
    EXPECT_EQ(block.value().state, BlockState::kCommitted);
    EXPECT_EQ(block.value().length, 128 * kMB);
}

TEST_F(BlockManagerTest, CommitBlockTwiceIsIdempotent) {
    auto alloc = block_mgr_->allocate_block(/*inode_id=*/100, /*block_index=*/0, /*replication=*/3);
    ASSERT_TRUE(alloc.hasValue());

    block_mgr_->commit_block(alloc.value().block_id,
                             128 * kMB,
                             alloc.value().generation_stamp,
                             datanode_ids(alloc.value()));
    auto second = block_mgr_->commit_block(alloc.value().block_id,
                                           128 * kMB,
                                           alloc.value().generation_stamp,
                                           datanode_ids(alloc.value()));
    ASSERT_TRUE(second.hasValue());

    auto conflicting = block_mgr_->commit_block(alloc.value().block_id,
                                                64 * kMB,
                                                alloc.value().generation_stamp,
                                                datanode_ids(alloc.value()));
    ASSERT_TRUE(conflicting.hasError());
}

TEST_F(BlockManagerTest, GetLocatedBlocks) {
    // Allocate and commit 2 blocks for the same inode.
    auto b0 = block_mgr_->allocate_block(100, 0, 3);
    ASSERT_TRUE(b0.hasValue());
    block_mgr_->commit_block(
        b0.value().block_id, 128 * kMB, b0.value().generation_stamp, datanode_ids(b0.value()));

    auto b1 = block_mgr_->allocate_block(100, 1, 3);
    ASSERT_TRUE(b1.hasValue());
    block_mgr_->commit_block(
        b1.value().block_id, 64 * kMB, b1.value().generation_stamp, datanode_ids(b1.value()));

    auto result = block_mgr_->get_located_blocks(100);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 2u);

    // Check offsets are sequential.
    EXPECT_EQ(result.value()[0].offset, 0u);
    EXPECT_EQ(result.value()[0].length, 128 * kMB);
    EXPECT_EQ(result.value()[1].offset, 128 * kMB);
    EXPECT_EQ(result.value()[1].length, 64 * kMB);
}

TEST_F(BlockManagerTest, GetLocatedBlocksNoCommitted) {
    // Allocate a block but don't commit — should return empty.
    block_mgr_->allocate_block(100, 0, 3);

    auto result = block_mgr_->get_located_blocks(100);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST_F(BlockManagerTest, ReportCorruptReplica) {
    auto alloc = block_mgr_->allocate_block(100, 0, 3);
    ASSERT_TRUE(alloc.hasValue());

    auto& locations = alloc.value().locations;
    ASSERT_FALSE(locations.empty());

    auto result =
        block_mgr_->report_corrupt_replica(alloc.value().block_id, locations[0].datanode_id);
    ASSERT_TRUE(result.hasValue());

    // Verify replica state changed.
    auto replicas = store_->get_replicas(alloc.value().block_id);
    bool found_corrupt = false;
    for (const auto& r : replicas.value()) {
        if (r.datanode_id == locations[0].datanode_id) {
            EXPECT_EQ(r.state, ReplicaState::kCorrupt);
            found_corrupt = true;
        }
    }
    EXPECT_TRUE(found_corrupt);
}

TEST_F(BlockManagerTest, CommitBlockOnlyFinalizesReportedReplicas) {
    auto alloc = block_mgr_->allocate_block(/*inode_id=*/100, /*block_index=*/0, /*replication=*/3);
    ASSERT_TRUE(alloc.hasValue());
    ASSERT_GE(alloc.value().locations.size(), 2u);

    uint64_t only_successful_dn = alloc.value().locations[0].datanode_id;
    auto result = block_mgr_->commit_block(
        alloc.value().block_id, 128 * kMB, alloc.value().generation_stamp, {only_successful_dn});
    ASSERT_TRUE(result.hasValue());

    auto replicas = store_->get_replicas(alloc.value().block_id);
    ASSERT_TRUE(replicas.hasValue());
    for (const auto& replica : replicas.value()) {
        if (replica.datanode_id == only_successful_dn) {
            EXPECT_EQ(replica.state, ReplicaState::kFinalized);
        } else {
            EXPECT_EQ(replica.state, ReplicaState::kWriting);
        }
    }
}

TEST_F(BlockManagerTest, GenerationStampMonotonic) {
    uint64_t g1 = block_mgr_->next_generation_stamp();
    uint64_t g2 = block_mgr_->next_generation_stamp();
    uint64_t g3 = block_mgr_->next_generation_stamp();
    EXPECT_LT(g1, g2);
    EXPECT_LT(g2, g3);
}

} // namespace
} // namespace pl::minidfs
