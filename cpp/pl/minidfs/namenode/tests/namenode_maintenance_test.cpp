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
// Created: 2026/06/01 23:01

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_set>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/time_util.h"
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/namenode_maintenance.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/replication_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

class NameNodeMaintenanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        namespace_manager_ = std::make_unique<NamespaceManager>(store_.get());
        datanode_manager_ = std::make_unique<DataNodeManager>(store_.get());
        placement_manager_ = std::make_unique<PlacementManager>(datanode_manager_.get());
        block_manager_ = std::make_unique<BlockManager>(store_.get(), placement_manager_.get());
        replication_manager_ =
            std::make_unique<ReplicationManager>(store_.get(), placement_manager_.get());
        maintenance_ = std::make_unique<NameNodeMaintenance>(NameNodeMaintenance::Config{},
                                                             store_.get(),
                                                             namespace_manager_.get(),
                                                             block_manager_.get(),
                                                             datanode_manager_.get(),
                                                             replication_manager_.get());
    }

    uint64_t register_datanode(std::string_view uuid) {
        auto result = datanode_manager_->register_datanode(
            uuid, uuid, uuid, 9000, 9100, "/rack", 1000 * kGB);
        EXPECT_TRUE(result.hasValue());
        return result.value();
    }

    void create_replica(uint64_t block_id, uint64_t datanode_id, ReplicaState state) {
        BlockReplica replica;
        replica.block_id = block_id;
        replica.datanode_id = datanode_id;
        replica.state = state;
        replica.length = 64;
        replica.generation_stamp = 7;
        replica.report_time_ms = now_ms();
        ASSERT_TRUE(store_->upsert_replica(replica).hasValue());
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<NamespaceManager> namespace_manager_;
    std::unique_ptr<DataNodeManager> datanode_manager_;
    std::unique_ptr<PlacementManager> placement_manager_;
    std::unique_ptr<BlockManager> block_manager_;
    std::unique_ptr<ReplicationManager> replication_manager_;
    std::unique_ptr<NameNodeMaintenance> maintenance_;
};

TEST_F(NameNodeMaintenanceTest, RecoversExpiredLeaseToCommittedPrefix) {
    uint64_t datanode_id = register_datanode("dn-1");
    auto file = namespace_manager_->create_file("/recover", "user", "group", 0644, 1, 128);
    ASSERT_TRUE(file.hasValue());

    Lease lease;
    lease.lease_id = 10;
    lease.inode_id = file.value().inode_id;
    lease.client_id = "dead-client";
    lease.state = LeaseState::kActive;
    lease.expire_time_ms = 1;
    ASSERT_TRUE(store_->create_lease(lease).hasValue());

    BlockMeta committed;
    committed.block_id = 20;
    committed.inode_id = file.value().inode_id;
    committed.block_index = 0;
    committed.generation_stamp = 7;
    committed.length = 64;
    committed.state = BlockState::kCommitted;
    committed.desired_replica = 1;
    ASSERT_TRUE(store_->create_block(committed).hasValue());
    create_replica(committed.block_id, datanode_id, ReplicaState::kFinalized);

    BlockMeta abandoned = committed;
    abandoned.block_id = 21;
    abandoned.block_index = 1;
    abandoned.length = 0;
    abandoned.state = BlockState::kAllocating;
    ASSERT_TRUE(store_->create_block(abandoned).hasValue());
    create_replica(abandoned.block_id, datanode_id, ReplicaState::kWriting);

    ASSERT_TRUE(maintenance_->run_once().hasValue());

    auto recovered = store_->get_inode(file.value().inode_id);
    ASSERT_TRUE(recovered.hasValue());
    EXPECT_EQ(recovered.value().state, FileState::kNormal);
    EXPECT_EQ(recovered.value().length, 64u);
    EXPECT_FALSE(store_->get_active_lease(file.value().inode_id).value().has_value());
    EXPECT_EQ(store_->get_block(abandoned.block_id).value().state, BlockState::kDeleted);
    EXPECT_EQ(store_->get_replicas(abandoned.block_id).value()[0].state, ReplicaState::kDeleting);
}

TEST_F(NameNodeMaintenanceTest, SchedulesDurableDeduplicatedRepairAwayFromDeadNode) {
    uint64_t source = register_datanode("dn-source");
    uint64_t dead = register_datanode("dn-dead");
    uint64_t target = register_datanode("dn-target");

    auto dead_node = store_->get_datanode(dead).value();
    dead_node.last_heartbeat_ms = 1;
    ASSERT_TRUE(store_->upsert_datanode(dead_node).hasValue());

    BlockMeta block;
    block.block_id = 30;
    block.inode_id = 40;
    block.block_index = 3;
    block.generation_stamp = 7;
    block.length = 64;
    block.state = BlockState::kCommitted;
    block.desired_replica = 2;
    ASSERT_TRUE(store_->create_block(block).hasValue());
    create_replica(block.block_id, source, ReplicaState::kFinalized);
    create_replica(block.block_id, dead, ReplicaState::kFinalized);

    ASSERT_TRUE(maintenance_->run_once().hasValue());
    ASSERT_TRUE(maintenance_->run_once().hasValue());

    auto tasks = maintenance_->take_replication_tasks(source);
    ASSERT_EQ(tasks.size(), 1u);
    EXPECT_EQ(tasks[0].target_datanode, target);
    EXPECT_EQ(tasks[0].inode_id, block.inode_id);
    EXPECT_EQ(tasks[0].block_index, block.block_index);
    EXPECT_EQ(tasks[0].generation_stamp, block.generation_stamp);

    auto replicas = store_->get_replicas(block.block_id).value();
    auto pending = std::find_if(replicas.begin(), replicas.end(), [target](const BlockReplica& r) {
        return r.datanode_id == target;
    });
    ASSERT_NE(pending, replicas.end());
    EXPECT_EQ(pending->state, ReplicaState::kWriting);

    ASSERT_TRUE(maintenance_->run_once().hasValue());
    EXPECT_EQ(maintenance_->take_replication_tasks(source).size(), 1u);

    ASSERT_TRUE(block_manager_
                    ->reconcile_block_report(
                        target,
                        {{.block_id = block.block_id,
                          .generation_stamp = block.generation_stamp,
                          .length = block.length}},
                        false)
                    .hasValue());
    ASSERT_TRUE(maintenance_->run_once().hasValue());
    EXPECT_TRUE(maintenance_->take_replication_tasks(source).empty());
}

TEST_F(NameNodeMaintenanceTest, RejectsWrongGenerationAndMarksMissingReplicaStale) {
    uint64_t datanode_id = register_datanode("dn-1");

    BlockMeta block;
    block.block_id = 50;
    block.generation_stamp = 7;
    block.state = BlockState::kCommitted;
    block.desired_replica = 1;
    ASSERT_TRUE(store_->create_block(block).hasValue());
    create_replica(block.block_id, datanode_id, ReplicaState::kWriting);

    ASSERT_TRUE(block_manager_
                    ->reconcile_block_report(
                        datanode_id,
                        {{.block_id = block.block_id, .generation_stamp = 8, .length = 64}},
                        false)
                    .hasValue());
    EXPECT_EQ(store_->get_replicas(block.block_id).value()[0].state, ReplicaState::kWriting);

    ASSERT_TRUE(block_manager_
                    ->reconcile_block_report(
                        datanode_id,
                        {{.block_id = block.block_id, .generation_stamp = 7, .length = 64}},
                        false)
                    .hasValue());
    EXPECT_EQ(store_->get_replicas(block.block_id).value()[0].state, ReplicaState::kFinalized);

    ASSERT_TRUE(block_manager_->reconcile_block_report(datanode_id, {}, true).hasValue());
    EXPECT_EQ(store_->get_replicas(block.block_id).value()[0].state, ReplicaState::kStale);
}

} // namespace
} // namespace pl::minidfs
