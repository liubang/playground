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

#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/minidfs/common/types.h"

namespace pl::minidfs {
namespace {

// ============================================================================
// Inode default values
// ============================================================================

TEST(TypesTest, InodeDefaults) {
    Inode inode;
    EXPECT_EQ(inode.inode_id, 0u);
    EXPECT_EQ(inode.type, InodeType::kDirectory);
    EXPECT_EQ(inode.parent_id, 0u);
    EXPECT_TRUE(inode.name.empty());
    EXPECT_TRUE(inode.owner.empty());
    EXPECT_TRUE(inode.group.empty());
    EXPECT_EQ(inode.permission, kDefaultPermission);
    EXPECT_EQ(inode.length, 0u);
    EXPECT_EQ(inode.replication, kDefaultReplication);
    EXPECT_EQ(inode.block_size, kDefaultBlockSize);
    EXPECT_EQ(inode.state, FileState::kNormal);
    EXPECT_EQ(inode.ctime_ms, 0u);
    EXPECT_EQ(inode.mtime_ms, 0u);
    EXPECT_EQ(inode.version, 0u);
}

TEST(TypesTest, InodeTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(InodeType::kDirectory), 1u);
    EXPECT_EQ(static_cast<uint8_t>(InodeType::kFile), 2u);
}

TEST(TypesTest, FileStateValues) {
    EXPECT_EQ(static_cast<uint8_t>(FileState::kNormal), 0u);
    EXPECT_EQ(static_cast<uint8_t>(FileState::kUnderConstruction), 1u);
    EXPECT_EQ(static_cast<uint8_t>(FileState::kDeleted), 2u);
}

// ============================================================================
// BlockMeta default values
// ============================================================================

TEST(TypesTest, BlockMetaDefaults) {
    BlockMeta bm;
    EXPECT_EQ(bm.block_id, 0u);
    EXPECT_EQ(bm.inode_id, 0u);
    EXPECT_EQ(bm.block_index, 0u);
    EXPECT_EQ(bm.generation_stamp, 0u);
    EXPECT_EQ(bm.length, 0u);
    EXPECT_EQ(bm.state, BlockState::kAllocating);
    EXPECT_EQ(bm.desired_replica, kDefaultReplication);
    EXPECT_EQ(bm.ctime_ms, 0u);
    EXPECT_EQ(bm.mtime_ms, 0u);
}

TEST(TypesTest, BlockStateValues) {
    EXPECT_EQ(static_cast<uint8_t>(BlockState::kAllocating), 0u);
    EXPECT_EQ(static_cast<uint8_t>(BlockState::kCommitted), 1u);
    EXPECT_EQ(static_cast<uint8_t>(BlockState::kCorrupt), 2u);
    EXPECT_EQ(static_cast<uint8_t>(BlockState::kDeleted), 3u);
}

// ============================================================================
// BlockReplica default values
// ============================================================================

TEST(TypesTest, BlockReplicaDefaults) {
    BlockReplica br;
    EXPECT_EQ(br.block_id, 0u);
    EXPECT_EQ(br.datanode_id, 0u);
    EXPECT_EQ(br.storage_id, 0u);
    EXPECT_EQ(br.state, ReplicaState::kWriting);
    EXPECT_EQ(br.length, 0u);
    EXPECT_EQ(br.generation_stamp, 0u);
    EXPECT_EQ(br.report_time_ms, 0u);
}

TEST(TypesTest, ReplicaStateValues) {
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kWriting), 0u);
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kFinalized), 1u);
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kCorrupt), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kStale), 3u);
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kDeleting), 4u);
    EXPECT_EQ(static_cast<uint8_t>(ReplicaState::kDeleted), 5u);
}

// ============================================================================
// DataNodeInfo default values
// ============================================================================

TEST(TypesTest, DataNodeInfoDefaults) {
    DataNodeInfo dn;
    EXPECT_EQ(dn.datanode_id, 0u);
    EXPECT_TRUE(dn.uuid.empty());
    EXPECT_TRUE(dn.hostname.empty());
    EXPECT_TRUE(dn.ip.empty());
    EXPECT_EQ(dn.rpc_port, 0u);
    EXPECT_EQ(dn.data_port, 0u);
    EXPECT_EQ(dn.rack, "/default-rack");
    EXPECT_EQ(dn.state, DataNodeState::kLive);
    EXPECT_EQ(dn.capacity_bytes, 0u);
    EXPECT_EQ(dn.used_bytes, 0u);
    EXPECT_EQ(dn.free_bytes, 0u);
    EXPECT_EQ(dn.last_heartbeat_ms, 0u);
}

TEST(TypesTest, DataNodeStateValues) {
    EXPECT_EQ(static_cast<uint8_t>(DataNodeState::kLive), 0u);
    EXPECT_EQ(static_cast<uint8_t>(DataNodeState::kStale), 1u);
    EXPECT_EQ(static_cast<uint8_t>(DataNodeState::kDead), 2u);
    EXPECT_EQ(static_cast<uint8_t>(DataNodeState::kDecommissioning), 3u);
    EXPECT_EQ(static_cast<uint8_t>(DataNodeState::kDecommissioned), 4u);
}

// ============================================================================
// Lease default values
// ============================================================================

TEST(TypesTest, LeaseDefaults) {
    Lease lease;
    EXPECT_EQ(lease.lease_id, 0u);
    EXPECT_EQ(lease.inode_id, 0u);
    EXPECT_TRUE(lease.client_id.empty());
    EXPECT_EQ(lease.state, LeaseState::kActive);
    EXPECT_EQ(lease.expire_time_ms, 0u);
    EXPECT_EQ(lease.ctime_ms, 0u);
    EXPECT_EQ(lease.mtime_ms, 0u);
}

TEST(TypesTest, LeaseStateValues) {
    EXPECT_EQ(static_cast<uint8_t>(LeaseState::kActive), 0u);
    EXPECT_EQ(static_cast<uint8_t>(LeaseState::kClosed), 1u);
}

// ============================================================================
// LocatedBlock / DataNodeEndpoint
// ============================================================================

TEST(TypesTest, LocatedBlockDefaults) {
    LocatedBlock lb;
    EXPECT_EQ(lb.block_id, 0u);
    EXPECT_EQ(lb.generation_stamp, 0u);
    EXPECT_EQ(lb.offset, 0u);
    EXPECT_EQ(lb.length, 0u);
    EXPECT_TRUE(lb.locations.empty());
}

TEST(TypesTest, DataNodeEndpointDefaults) {
    DataNodeEndpoint ep;
    EXPECT_EQ(ep.datanode_id, 0u);
    EXPECT_TRUE(ep.host.empty());
    EXPECT_EQ(ep.data_port, 0u);
}

// ============================================================================
// FileStatus
// ============================================================================

TEST(TypesTest, FileStatusDefaults) {
    FileStatus fs;
    EXPECT_EQ(fs.inode_id, 0u);
    EXPECT_TRUE(fs.path.empty());
    EXPECT_FALSE(fs.is_dir);
    EXPECT_EQ(fs.length, 0u);
    EXPECT_EQ(fs.replication, 0u);
    EXPECT_EQ(fs.block_size, 0u);
    EXPECT_EQ(fs.mtime_ms, 0u);
    EXPECT_TRUE(fs.owner.empty());
    EXPECT_TRUE(fs.group.empty());
    EXPECT_EQ(fs.permission, 0u);
}

// ============================================================================
// Struct assignment / copy
// ============================================================================

TEST(TypesTest, InodeCopy) {
    Inode a;
    a.inode_id = 42;
    a.name = "test";
    a.type = InodeType::kFile;
    a.owner = "user1";

    Inode b = a;
    EXPECT_EQ(b.inode_id, 42u);
    EXPECT_EQ(b.name, "test");
    EXPECT_EQ(b.type, InodeType::kFile);
    EXPECT_EQ(b.owner, "user1");
}

TEST(TypesTest, LocatedBlockWithLocations) {
    LocatedBlock lb;
    lb.block_id = 100;
    lb.generation_stamp = 5000;
    lb.offset = 0;
    lb.length = 128 * kMB;
    lb.locations.push_back({.datanode_id = 1, .host = "dn1", .data_port = 9001});
    lb.locations.push_back({.datanode_id = 2, .host = "dn2", .data_port = 9002});

    EXPECT_EQ(lb.locations.size(), 2u);
    EXPECT_EQ(lb.locations[0].host, "dn1");
    EXPECT_EQ(lb.locations[1].data_port, 9002u);
}

} // namespace
} // namespace pl::minidfs
