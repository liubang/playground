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

#include "cpp/pl/minidfs/common/constants.h"

#include <gtest/gtest.h>

namespace pl::minidfs {
namespace {

// ============================================================================
// Size constants
// ============================================================================

TEST(ConstantsTest, SizeUnits) {
    EXPECT_EQ(kKB, 1024ULL);
    EXPECT_EQ(kMB, 1024ULL * 1024ULL);
    EXPECT_EQ(kGB, 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(kTB, 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(kPB, 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(ConstantsTest, SizeRelationships) {
    EXPECT_EQ(kMB, 1024 * kKB);
    EXPECT_EQ(kGB, 1024 * kMB);
    EXPECT_EQ(kTB, 1024 * kGB);
    EXPECT_EQ(kPB, 1024 * kTB);
}

// ============================================================================
// Block format constants
// ============================================================================

TEST(ConstantsTest, BlockMagic) {
    // "MDFS" = 0x4D444653
    EXPECT_EQ(kBlockMagic, 0x4D444653u);
}

TEST(ConstantsTest, BlockFormatVersion) {
    EXPECT_EQ(kBlockFormatVersion, 1u);
}

TEST(ConstantsTest, MaxChunkCount) {
    EXPECT_EQ(kMaxChunkCount, 256u);
}

TEST(ConstantsTest, DefaultChunkSize) {
    EXPECT_EQ(kDefaultChunkSize, 1 * kMB);
}

// ============================================================================
// Filesystem defaults
// ============================================================================

TEST(ConstantsTest, DefaultBlockSize) {
    EXPECT_EQ(kDefaultBlockSize, 128 * kMB);
}

TEST(ConstantsTest, DefaultReplication) {
    EXPECT_EQ(kDefaultReplication, 3u);
}

TEST(ConstantsTest, MinWriteReplica) {
    EXPECT_EQ(kMinWriteReplica, 2u);
    EXPECT_LE(kMinWriteReplica, kDefaultReplication);
}

TEST(ConstantsTest, DefaultPermission) {
    EXPECT_EQ(kDefaultPermission, 0755u);
}

// ============================================================================
// DataNode timing
// ============================================================================

TEST(ConstantsTest, HeartbeatInterval) {
    EXPECT_EQ(kDefaultHeartbeatIntervalMs, 3000u);
}

TEST(ConstantsTest, StaleTimeout) {
    EXPECT_EQ(kDefaultStaleTimeoutMs, 30000u);
    EXPECT_GT(kDefaultStaleTimeoutMs, kDefaultHeartbeatIntervalMs);
}

TEST(ConstantsTest, DeadTimeout) {
    EXPECT_EQ(kDefaultDeadTimeoutMs, 600000u);
    EXPECT_GT(kDefaultDeadTimeoutMs, kDefaultStaleTimeoutMs);
}

TEST(ConstantsTest, BlockReportInterval) {
    EXPECT_EQ(kDefaultBlockReportIntervalMs, 600000u);
}

// ============================================================================
// Replication
// ============================================================================

TEST(ConstantsTest, ReplicationScanInterval) {
    EXPECT_EQ(kDefaultReplicationScanIntervalMs, 30000u);
}

TEST(ConstantsTest, MaxReplicationTasks) {
    EXPECT_EQ(kDefaultMaxReplicationTasksPerRound, 100u);
}

// ============================================================================
// Lease
// ============================================================================

TEST(ConstantsTest, LeaseTimeout) {
    EXPECT_EQ(kDefaultLeaseTimeoutMs, 60000u);
}

// ============================================================================
// Namespace
// ============================================================================

TEST(ConstantsTest, MaxPathLength) {
    EXPECT_EQ(kMaxPathLength, 8192u);
}

TEST(ConstantsTest, MaxNameLength) {
    EXPECT_EQ(kMaxNameLength, 255u);
}

TEST(ConstantsTest, RootInodeId) {
    EXPECT_EQ(kRootInodeId, 1u);
}

} // namespace
} // namespace pl::minidfs
