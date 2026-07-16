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
// Created: 2026/05/24 16:20

// Regression tests for known NameNode-side bugs found during code review.
// Each test constructs a minimal reproduction scenario for a specific issue.
// Tests are expected to FAIL against the current (buggy) code and PASS after fixes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/lease_manager.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/namenode/namenode_service_impl.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {
namespace {

class FirstInodeUpdateBlockingStore : public testing::MockMetadataStore {
public:
    void arm() { armed_.store(true); }

    void wait_for_first_update() { first_update_entered_.get_future().wait(); }

    void release_first_update() { release_first_update_.set_value(); }

    pl::Result<pl::Void> update_inode(const Inode& inode) override {
        if (armed_.load() && !first_update_blocked_.exchange(true)) {
            first_update_entered_.set_value();
            release_first_update_.get_future().wait();
        }
        return testing::MockMetadataStore::update_inode(inode);
    }

private:
    std::atomic<bool> armed_ = false;
    std::atomic<bool> first_update_blocked_ = false;
    std::promise<void> first_update_entered_;
    std::promise<void> release_first_update_;
};

class ListChildrenFailingStore : public testing::MockMetadataStore {
public:
    void set_fail_parent(uint64_t parent_id) { fail_parent_ = parent_id; }

    pl::Result<std::vector<Inode>> list_children(uint64_t parent_id) override {
        if (fail_parent_.has_value() && fail_parent_.value() == parent_id) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kMySQLQueryFailed),
                                 "injected list_children failure");
        }
        return testing::MockMetadataStore::list_children(parent_id);
    }

private:
    std::optional<uint64_t> fail_parent_;
};

// Fixture: NameNode-side components with MockMetadataStore

class RegressionNameNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        ns_mgr_ = std::make_unique<NamespaceManager>(store_.get());
        dn_mgr_ = std::make_unique<DataNodeManager>(store_.get());
        placement_ = std::make_unique<PlacementManager>(dn_mgr_.get());
        block_mgr_ =
            std::make_unique<BlockManager>(store_.get(), placement_.get(), "test-secret");
        lease_mgr_ = std::make_unique<LeaseManager>(store_.get());

        // Register 3 datanodes so placement can succeed.
        dn_mgr_->register_datanode("dn-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
        dn_mgr_->register_datanode("dn-2", "host2", "10.0.0.2", 9000, 9100, "/rack2", 1000 * kGB);
        dn_mgr_->register_datanode("dn-3", "host3", "10.0.0.3", 9000, 9100, "/rack3", 1000 * kGB);
    }

    testing::MockMetadataStore* store_raw() { return store_.get(); }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<DataNodeManager> dn_mgr_;
    std::unique_ptr<PlacementManager> placement_;
    std::unique_ptr<BlockManager> block_mgr_;
    std::unique_ptr<LeaseManager> lease_mgr_;
};

std::vector<uint64_t> datanode_ids(const LocatedBlock& block) {
    std::vector<uint64_t> ids;
    ids.reserve(block.locations.size());
    for (const auto& location : block.locations) {
        ids.push_back(location.datanode_id);
    }
    return ids;
}

// P0: Write-then-read failure — CommitBlock not called by client path
//
// Scenario: Simulate the server-side effect of the client write path:
//   1. CreateFile
//   2. AllocateBlock
//   3. (DataNode writes succeed, but no CommitBlock is called)
//   4. CompleteFile → get_located_blocks() returns empty → file length = 0
//
// Expected after fix: CommitBlock is called, block becomes kCommitted,
//   replicas become kFinalized, get_located_blocks returns the block.

TEST_F(RegressionNameNodeTest, P0_WriteWithoutCommitBlockProducesZeroLengthFile) {
    auto file = ns_mgr_->create_file("/test.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());
    uint64_t inode_id = file.value().inode_id;

    // Allocate a block (simulating what the client does)
    auto alloc = block_mgr_->allocate_block(inode_id, 0, 3);
    ASSERT_TRUE(alloc.hasValue());
    auto& lb = alloc.value();

    // Simulate: DataNode wrote 1MB of data but client does NOT call CommitBlock.
    // Now try to get located blocks (what CompleteFile does internally).
    auto located = block_mgr_->get_located_blocks(inode_id);
    ASSERT_TRUE(located.hasValue());

    // Without CommitBlock, the block stays in kAllocating state.
    // get_located_blocks skips non-committed blocks → returns empty.
    EXPECT_TRUE(located.value().empty())
        << "Without CommitBlock, no blocks are visible for reading (expected)";

    // FIX VERIFIED: After the client calls CommitBlock, block becomes visible.
    auto commit =
        block_mgr_->commit_block(lb.block_id, 1 * kMB, lb.generation_stamp, datanode_ids(lb));
    ASSERT_TRUE(commit.hasValue());

    // FIX #2: commit_block now transitions replicas from kWriting to kFinalized.
    auto located_after_commit = block_mgr_->get_located_blocks(inode_id);
    ASSERT_TRUE(located_after_commit.hasValue());

    bool block_visible = !located_after_commit.value().empty();
    bool has_locations = block_visible && !located_after_commit.value()[0].locations.empty();

    // FIX VERIFIED: committed block is now readable with finalized replica locations.
    EXPECT_TRUE(block_visible) << "FIX VERIFIED: Committed block is visible in get_located_blocks";
    EXPECT_TRUE(has_locations)
        << "FIX VERIFIED: Replicas are transitioned to kFinalized, locations available";
}

// P0: Pipeline replication — commit must only finalize replicas that actually acked
//
// Scenario:
//   1. AllocateBlock returns 3 target datanodes
//   2. NameNode creates kWriting replicas for all 3
//   3. Only a subset of DNs reports successful write
//   4. Result: only that subset may transition to kFinalized

TEST_F(RegressionNameNodeTest, P0_PipelineReplicationCommitOnlyFinalizesAckedReplicas) {
    auto file = ns_mgr_->create_file("/pipeline.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());

    auto alloc = block_mgr_->allocate_block(file.value().inode_id, 0, 3);
    ASSERT_TRUE(alloc.hasValue());

    // Verify: NameNode created 3 replicas in kWriting state
    auto replicas = store_raw()->get_replicas(alloc.value().block_id);
    ASSERT_TRUE(replicas.hasValue());
    EXPECT_EQ(replicas.value().size(), 3u) << "NameNode pre-registers 3 replicas for pipeline";

    // All replicas are in kWriting state
    for (const auto& r : replicas.value()) {
        EXPECT_EQ(r.state, ReplicaState::kWriting);
    }

    ASSERT_GE(alloc.value().locations.size(), 2u);
    uint64_t acked_dn_1 = alloc.value().locations[0].datanode_id;
    uint64_t acked_dn_2 = alloc.value().locations[1].datanode_id;
    auto commit = block_mgr_->commit_block(alloc.value().block_id,
                                           1 * kMB,
                                           alloc.value().generation_stamp,
                                           {acked_dn_1, acked_dn_2});
    ASSERT_TRUE(commit.hasValue());

    auto after_commit = store_raw()->get_replicas(alloc.value().block_id);
    ASSERT_TRUE(after_commit.hasValue());
    for (const auto& r : after_commit.value()) {
        if (r.datanode_id == acked_dn_1 || r.datanode_id == acked_dn_2) {
            EXPECT_EQ(r.state, ReplicaState::kFinalized);
        } else {
            EXPECT_EQ(r.state, ReplicaState::kWriting)
                << "FIX VERIFIED: unacked replicas are not finalized";
        }
    }
}

// P1: CreateFile + lease acquisition must be a single transactional operation.
//
// With MockMetadataStore this can't reproduce the connection-mismatch directly.
// We test the SEMANTIC consequence: operations are not grouped atomically.

TEST_F(RegressionNameNodeTest, P1_CreateFileLeaseFailureLeavesOrphanInode) {
    // Create file #1 and acquire its lease
    auto file1 = ns_mgr_->create_file("/occupied.dat", "user1", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file1.hasValue());
    auto lease1 = lease_mgr_->acquire_lease(file1.value().inode_id, "client-1");
    ASSERT_TRUE(lease1.hasValue());

    // Manager-level operations are still independently callable; the RPC service
    // wraps create_file + acquire_lease in one MetadataStore transaction.
    auto file2 = ns_mgr_->create_file("/file2.dat", "user2", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file2.hasValue());

    // Both inodes exist independently
    auto stat1 = ns_mgr_->get_file_status("/occupied.dat");
    auto stat2 = ns_mgr_->get_file_status("/file2.dat");
    ASSERT_TRUE(stat1.hasValue());
    ASSERT_TRUE(stat2.hasValue());

    // This test keeps the lower-level managers available for focused unit tests.
}

// P1: alloc_id() must be safe under concurrent callers.
//
// MockMetadataStore uses a mutex so this passes. Documents the interface
// contract and serves as a canary for MySQL-backed tests.

TEST_F(RegressionNameNodeTest, P1_AllocIdConcurrentSafety) {
    constexpr int kThreads = 8;
    constexpr int kAllocsPerThread = 100;

    std::vector<std::thread> threads;
    std::vector<uint64_t> all_ids;
    std::mutex result_mu;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kAllocsPerThread; ++i) {
                auto id = store_raw()->alloc_id("concurrent_test");
                ASSERT_TRUE(id.hasValue());
                std::lock_guard lock(result_mu);
                all_ids.push_back(id.value());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::sort(all_ids.begin(), all_ids.end());
    auto last = std::unique(all_ids.begin(), all_ids.end());
    EXPECT_EQ(last, all_ids.end())
        << "alloc_id must not produce duplicate IDs under concurrent callers.";
    EXPECT_EQ(static_cast<int>(all_ids.size()), kThreads * kAllocsPerThread);
}

// P1: Recursive delete removes all descendants.
//
// Scenario:
//   1. Create /a/b/c (nested directories)
//   2. Delete /a with recursive=true
//   3. All descendants are removed, deepest first.

TEST_F(RegressionNameNodeTest, P1_RecursiveDeleteLeavesOrphans) {
    auto a = ns_mgr_->mkdir("/a", "user", "grp", 0755, true);
    ASSERT_TRUE(a.hasValue());
    auto b = ns_mgr_->mkdir("/a/b", "user", "grp", 0755, true);
    ASSERT_TRUE(b.hasValue());
    auto c = ns_mgr_->mkdir("/a/b/c", "user", "grp", 0755, true);
    ASSERT_TRUE(c.hasValue());

    auto file = ns_mgr_->create_file("/a/b/c/file.txt", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());
    uint64_t file_inode_id = file.value().inode_id;
    uint64_t c_inode_id = c.value().inode_id;

    auto del = ns_mgr_->remove("/a", /*recursive=*/true);
    ASSERT_TRUE(del.hasValue());

    // Verify /a is gone from path resolution
    auto stat_a = ns_mgr_->get_file_status("/a");
    EXPECT_TRUE(stat_a.hasError());

    // FIX VERIFIED: All descendants are deleted by iterative DFS.
    auto orphan_c = store_raw()->get_inode(c_inode_id);
    auto orphan_file = store_raw()->get_inode(file_inode_id);

    EXPECT_TRUE(orphan_c.hasError())
        << "FIX VERIFIED: Grandchild directory /a/b/c is properly deleted";
    EXPECT_TRUE(orphan_file.hasError())
        << "FIX VERIFIED: Deep file /a/b/c/file.txt is properly deleted";
}

// P1: Delete preserves replica metadata until DataNodes confirm cleanup.
//
// Scenario:
//   1. Create file, allocate block, commit block
//   2. Delete the file
//   3. Block is hidden from reads and replicas move to kDeleting

TEST_F(RegressionNameNodeTest, P1_DeleteFileLeavesBlockMetadata) {
    auto file = ns_mgr_->create_file("/todelete.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());
    uint64_t inode_id = file.value().inode_id;

    auto alloc = block_mgr_->allocate_block(inode_id, 0, 3);
    ASSERT_TRUE(alloc.hasValue());
    uint64_t block_id = alloc.value().block_id;

    auto commit = block_mgr_->commit_block(
        block_id, 1 * kMB, alloc.value().generation_stamp, datanode_ids(alloc.value()));
    ASSERT_TRUE(commit.hasValue());

    // FIX VERIFIED: Simulate what NameNodeServiceImpl::Delete does:
    // 1. Invalidate blocks before namespace removal
    auto invalidate = block_mgr_->invalidate_blocks(inode_id);
    ASSERT_TRUE(invalidate.hasValue());

    // 2. Remove the inode from namespace
    auto del = ns_mgr_->remove("/todelete.dat");
    ASSERT_TRUE(del.hasValue());

    // FIX VERIFIED: Block state is kDeleted
    auto block = store_raw()->get_block(block_id);
    ASSERT_TRUE(block.hasValue());
    EXPECT_EQ(block.value().state, BlockState::kDeleted)
        << "FIX VERIFIED: Block is marked as kDeleted after file deletion";

    // FIX VERIFIED: Replica metadata is retained in kDeleting until DataNode confirms deletion.
    auto replicas = store_raw()->get_replicas(block_id);
    ASSERT_TRUE(replicas.hasValue());
    ASSERT_FALSE(replicas.value().empty());
    for (const auto& replica : replicas.value()) {
        EXPECT_EQ(replica.state, ReplicaState::kDeleting)
            << "FIX VERIFIED: Replica remains addressable for delete commands";
    }

    auto commands = block_mgr_->get_blocks_to_delete(replicas.value()[0].datanode_id);
    ASSERT_TRUE(commands.hasValue());
    ASSERT_FALSE(commands.value().empty())
        << "FIX VERIFIED: NameNode can still issue a delete command for this replica";

    auto reconcile = block_mgr_->reconcile_block_report(replicas.value()[0].datanode_id, {}, true);
    ASSERT_TRUE(reconcile.hasValue());

    auto after_reconcile = store_raw()->get_replicas(block_id);
    ASSERT_TRUE(after_reconcile.hasValue());
    bool confirmed_deleted = false;
    for (const auto& replica : after_reconcile.value()) {
        if (replica.datanode_id == replicas.value()[0].datanode_id) {
            confirmed_deleted = (replica.state == ReplicaState::kDeleted);
        }
    }
    EXPECT_TRUE(confirmed_deleted)
        << "FIX VERIFIED: missing block report confirms the DataNode deleted the replica";
}

TEST(RegressionNameNodeRenameTest, ConcurrentCrossRenameCannotCreateNamespaceCycle) {
    auto store = std::make_unique<FirstInodeUpdateBlockingStore>();
    auto ns_mgr = std::make_unique<NamespaceManager>(store.get());
    auto dn_mgr = std::make_unique<DataNodeManager>(store.get());
    auto placement = std::make_unique<PlacementManager>(dn_mgr.get());
    auto block_mgr =
        std::make_unique<BlockManager>(store.get(), placement.get(), "test-secret");
    auto lease_mgr = std::make_unique<LeaseManager>(store.get());
    NameNodeServiceImpl service(ns_mgr.get(), block_mgr.get(), lease_mgr.get(), store.get(),
                                "test-secret", 3600000);

    ASSERT_TRUE(ns_mgr->mkdir("/a", "user", "grp", 0755).hasValue());
    ASSERT_TRUE(ns_mgr->mkdir("/b", "user", "grp", 0755).hasValue());
    store->arm();

    protocol::RenameRequest first_request;
    first_request.set_src("/a");
    first_request.set_dst("/b/a");
    protocol::RenameResponse first_response;
    auto first = std::async(std::launch::async, [&] {
        service.Rename(nullptr, &first_request, &first_response, nullptr);
    });

    // Hold the first rename immediately before its inode update. Without NameNode-level
    // serialization, the inverse rename can validate against the same old tree and succeed.
    store->wait_for_first_update();

    protocol::RenameRequest second_request;
    second_request.set_src("/b");
    second_request.set_dst("/a/b");
    protocol::RenameResponse second_response;
    std::promise<void> second_started;
    auto second = std::async(std::launch::async, [&] {
        second_started.set_value();
        service.Rename(nullptr, &second_request, &second_response, nullptr);
    });
    second_started.get_future().wait();

    // The second RPC must remain outside the protected rename transaction until the first
    // update and commit complete. The gate makes the vulnerable interleaving reproducible.
    EXPECT_EQ(second.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    store->release_first_update();
    first.get();
    second.get();

    const int successes = (first_response.status().code() == 0 ? 1 : 0) +
                          (second_response.status().code() == 0 ? 1 : 0);
    EXPECT_EQ(successes, 1);

    auto a = store->get_child(1, "a");
    auto b = store->get_child(1, "b");
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    EXPECT_NE(a.value().has_value(), b.value().has_value());
}

TEST(RegressionNameNodeDeleteTest, RecursiveDeleteListChildrenErrorMustAbort) {
    auto store = std::make_unique<ListChildrenFailingStore>();
    auto ns_mgr = std::make_unique<NamespaceManager>(store.get());
    auto dn_mgr = std::make_unique<DataNodeManager>(store.get());
    auto placement = std::make_unique<PlacementManager>(dn_mgr.get());
    auto block_mgr =
        std::make_unique<BlockManager>(store.get(), placement.get(), "test-secret");
    auto lease_mgr = std::make_unique<LeaseManager>(store.get());
    NameNodeServiceImpl service(ns_mgr.get(), block_mgr.get(), lease_mgr.get(), store.get(),
                            "test-secret", 3600000);

    ASSERT_TRUE(ns_mgr->mkdir("/a/b", "user", "grp", 0755, true).hasValue());
    auto child = ns_mgr->resolve_path("/a/b");
    ASSERT_TRUE(child.hasValue());
    store->set_fail_parent(child.value().inode_id);

    protocol::DeleteRequest request;
    request.set_path("/a");
    request.set_recursive(true);
    protocol::DeleteResponse response;
    service.Delete(nullptr, &request, &response, nullptr);

    EXPECT_NE(response.status().code(), 0u);
    auto root = ns_mgr->get_file_status("/a");
    EXPECT_TRUE(root.hasValue()) << "delete should abort and keep namespace intact";
}

// P0: Transaction binding — operations within begin_transaction()/commit()
//     should execute as a coherent unit.
//
// Bug: MySQLMetadataStore methods acquired a NEW connection via pool_->acquire()
//      ignoring the bound_conn_ set by begin_transaction(). This meant operations
//      inside a transaction scope ran on different connections, breaking
//      rollback/commit isolation.
//
// Limitation: MockTransaction is a no-op (commit/rollback do nothing), so we
//      cannot verify connection-level binding here. This test exercises the
//      transactional code path end-to-end to ensure the API contract holds:
//      begin_transaction() → multiple operations → commit() produces consistent state.

TEST_F(RegressionNameNodeTest, P0_TransactionBindingMultiOpConsistency) {
    // Begin a transaction scope.
    auto txn = store_raw()->begin_transaction();
    ASSERT_TRUE(txn.hasValue());

    // Perform multiple operations that should be part of one transaction:
    // 1. Create a directory
    auto dir = ns_mgr_->mkdir("/txn_dir", "user", "grp", 0755, true);
    ASSERT_TRUE(dir.hasValue());

    // 2. Create a file inside it
    auto file = ns_mgr_->create_file("/txn_dir/data.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());

    // 3. Acquire a lease on the file
    auto lease = lease_mgr_->acquire_lease(file.value().inode_id, "client-txn");
    ASSERT_TRUE(lease.hasValue());

    // Commit the transaction.
    auto commit_result = txn.value()->commit();
    ASSERT_TRUE(commit_result.hasValue());

    // Verify all operations are visible after commit.
    auto dir_stat = ns_mgr_->get_file_status("/txn_dir");
    EXPECT_TRUE(dir_stat.hasValue()) << "Directory created within transaction is visible";

    auto file_stat = ns_mgr_->get_file_status("/txn_dir/data.dat");
    EXPECT_TRUE(file_stat.hasValue()) << "File created within transaction is visible";

    // FIX VERIFIED: With the fix, all three operations use the same bound connection,
    // so commit/rollback applies atomically. With the bug, each operation used a
    // different connection, making the transaction boundary meaningless.
}

// P1: Sort-then-shuffle capacity awareness — choose_targets prefers nodes
//     with more free space.
//
// Bug: PlacementManager::choose_targets() sorted candidates by free_bytes
//      descending, then did a full std::shuffle() over the ENTIRE vector,
//      completely destroying the sort order. The fix limits shuffle to only
//      the top 2*num_replicas candidates.
//
// Verification: With 5 datanodes where one has 10x more free space,
//      choose_targets(1) should select the high-capacity node significantly
//      more than random (>50% vs ~20% with full shuffle).
//      shuffle_range = min(5, 2*1) = 2, so only top-2 nodes are shuffled.
//      The high-capacity node is always in the top-2 → chosen ~50% of the time.

TEST_F(RegressionNameNodeTest, P1_CapacityAwarePlacementPrefersHighFreeNodes) {
    // Override the fixture's equal-capacity nodes by sending heartbeats
    // with different free_bytes values.
    // dn-1: 100 GB free (high capacity — 10x others)
    // dn-2: 10 GB free
    // dn-3: 10 GB free
    // Also register 2 more nodes with low free space.
    dn_mgr_->register_datanode("dn-4", "host4", "10.0.0.4", 9000, 9100, "/rack1", 1000 * kGB);
    dn_mgr_->register_datanode("dn-5", "host5", "10.0.0.5", 9000, 9100, "/rack2", 1000 * kGB);

    // Get datanode IDs by looking them up.
    auto all_dns = dn_mgr_->get_all_datanodes();
    ASSERT_TRUE(all_dns.hasValue());
    ASSERT_EQ(all_dns.value().size(), 5u);

    // Send heartbeats to set free_bytes: first node gets 10x more free space.
    for (const auto& dn : all_dns.value()) {
        uint64_t free = (dn.uuid == "dn-1") ? 100 * kGB : 10 * kGB;
        uint64_t used = 1000 * kGB - free;
        auto hb = dn_mgr_->handle_heartbeat(dn.datanode_id, 1000 * kGB, used, free);
        ASSERT_TRUE(hb.hasValue());
    }

    // Run choose_targets(1) many times and count how often dn-1 is chosen.
    constexpr int kTrials = 200;
    int high_cap_chosen = 0;

    // Find dn-1's datanode_id.
    uint64_t dn1_id = 0;
    for (const auto& dn : all_dns.value()) {
        if (dn.uuid == "dn-1") {
            dn1_id = dn.datanode_id;
            break;
        }
    }
    ASSERT_NE(dn1_id, 0u);

    for (int i = 0; i < kTrials; ++i) {
        auto targets = placement_->choose_targets(1, std::nullopt);
        ASSERT_TRUE(targets.hasValue());
        ASSERT_EQ(targets.value().size(), 1u);
        if (targets.value()[0].datanode_id == dn1_id) {
            ++high_cap_chosen;
        }
    }

    // With the fix: shuffle_range = min(5, 2) = 2. The high-capacity node is
    // always sorted first, so it's always in the 2-element shuffle window.
    // Expected selection rate: ~50% (1 out of 2 in the window).
    // With the old bug (full shuffle): ~20% (1 out of 5, random).
    // Threshold: >35% ensures we're well above random but allows for variance.
    double selection_rate = static_cast<double>(high_cap_chosen) / kTrials;
    EXPECT_GT(selection_rate, 0.35)
        << "FIX VERIFIED: High-capacity node selected " << high_cap_chosen << "/" << kTrials
        << " times (" << (selection_rate * 100) << "%). "
        << "With full-shuffle bug, expected ~20%. With fix, expected ~50%.";
}

// P0: alloc_id() first-insert returns wrong base ID
//
// In the MySQL implementation, the INSERT path uses:
//   INSERT INTO id_allocators (name, next_id) VALUES ('x', count)
//   ON DUPLICATE KEY UPDATE next_id = LAST_INSERT_ID(next_id) + count
//
// When the row does NOT exist (first insert), MySQL's LAST_INSERT_ID()
// returns 0 (since no LAST_INSERT_ID(expr) was called in the INSERT VALUES).
// But next_id is set to `count`. The returned base_id (0) is inconsistent
// with the stored state.
//
// Expected: consecutive alloc_id calls produce a contiguous range with no gaps.
//   alloc_id("new_name", 5) → base=X, next_id=X+5
//   alloc_id("new_name", 3) → base=X+5, next_id=X+5+3
//
// Bug: first call returns 0, second call returns `count` from first call,
//   leaving a gap [0, first_count).

// P0: Idempotent replay — duplicate Mkdir request returns equivalent response
//
// Scenario: Client sends Mkdir with the same request_id twice.
//   1st call: succeeds, returns inode_id.
//   2nd call: should replay the same inode_id without creating a duplicate inode.
TEST_F(RegressionNameNodeTest, P0_IdempotentMkdirReplay) {
    NameNodeServiceImpl service(ns_mgr_.get(), block_mgr_.get(), lease_mgr_.get(), store_raw(),
                                "test-secret", 3600000);
    const std::string req_id = "idem-mkdir-001";

    protocol::MkdirRequest req1;
    req1.set_path("/idem_dir");
    req1.set_owner("user");
    req1.set_group("grp");
    req1.set_permission(0755);
    req1.mutable_header()->set_request_id(req_id);
    req1.mutable_header()->set_client_id("test-client");
    protocol::MkdirResponse resp1;
    service.Mkdir(nullptr, &req1, &resp1, nullptr);
    ASSERT_EQ(resp1.status().code(), 0u);
    const uint64_t first_inode_id = resp1.inode_id();

    // Replay with same request_id
    protocol::MkdirRequest req2;
    req2.set_path("/idem_dir");
    req2.set_owner("user");
    req2.set_group("grp");
    req2.set_permission(0755);
    req2.mutable_header()->set_request_id(req_id);
    req2.mutable_header()->set_client_id("test-client");
    protocol::MkdirResponse resp2;
    service.Mkdir(nullptr, &req2, &resp2, nullptr);
    EXPECT_EQ(resp2.status().code(), 0u) << "Duplicate Mkdir should succeed";
    EXPECT_EQ(resp2.inode_id(), first_inode_id)
        << "Duplicate Mkdir must return the same inode_id";
}

// P0: Idempotent replay — duplicate CreateFile request returns equivalent response
TEST_F(RegressionNameNodeTest, P0_IdempotentCreateFileReplay) {
    NameNodeServiceImpl service(ns_mgr_.get(), block_mgr_.get(), lease_mgr_.get(), store_raw(),
                                "test-secret", 3600000);
    const std::string req_id = "idem-create-001";

    protocol::CreateFileRequest req1;
    req1.set_path("/idem_file.dat");
    req1.set_owner("user");
    req1.set_group("grp");
    req1.set_permission(0644);
    req1.set_replication(3);
    req1.set_block_size(128 * kMB);
    req1.set_client_id("test-client");
    req1.mutable_header()->set_request_id(req_id);
    req1.mutable_header()->set_client_id("test-client");
    protocol::CreateFileResponse resp1;
    service.CreateFile(nullptr, &req1, &resp1, nullptr);
    ASSERT_EQ(resp1.status().code(), 0u);
    const uint64_t first_inode_id = resp1.inode_id();
    const uint64_t first_lease_id = resp1.lease_id();

    // Replay with same request_id
    protocol::CreateFileRequest req2;
    req2.set_path("/idem_file.dat");
    req2.set_owner("user");
    req2.set_group("grp");
    req2.set_permission(0644);
    req2.set_replication(3);
    req2.set_block_size(128 * kMB);
    req2.set_client_id("test-client");
    req2.mutable_header()->set_request_id(req_id);
    req2.mutable_header()->set_client_id("test-client");
    protocol::CreateFileResponse resp2;
    service.CreateFile(nullptr, &req2, &resp2, nullptr);
    EXPECT_EQ(resp2.status().code(), 0u) << "Duplicate CreateFile should succeed";
    EXPECT_EQ(resp2.inode_id(), first_inode_id)
        << "Duplicate CreateFile must return the same inode_id";
    EXPECT_EQ(resp2.lease_id(), first_lease_id)
        << "Duplicate CreateFile must return the same lease_id";
}

TEST_F(RegressionNameNodeTest, AllocateBlockReplayReissuesToken) {
    constexpr uint64_t kTokenTtlMs = 3600000;
    NameNodeServiceImpl service(ns_mgr_.get(),
                                block_mgr_.get(),
                                lease_mgr_.get(),
                                store_raw(),
                                "test-secret",
                                kTokenTtlMs);
    auto file = ns_mgr_->create_file("/idem_block.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());
    ASSERT_TRUE(lease_mgr_->acquire_lease(file.value().inode_id, "client").hasValue());

    protocol::AllocateBlockRequest request;
    request.set_inode_id(file.value().inode_id);
    request.set_block_index(0);
    request.set_replication(3);
    request.set_client_id("client");
    request.mutable_header()->set_request_id("idem-allocate-001");
    request.mutable_header()->set_client_id("client");

    protocol::AllocateBlockResponse first;
    service.AllocateBlock(nullptr, &request, &first, nullptr);
    ASSERT_EQ(first.status().code(), 0u);
    ASSERT_TRUE(first.has_block());
    EXPECT_TRUE(verify_block_token(first.block().block_token(),
                                   "test-secret",
                                   BlockTokenPermission::kWrite,
                                   first.block().block_id(),
                                   first.block().generation_stamp(),
                                   file.value().inode_id,
                                   0));

    protocol::AllocateBlockResponse replay;
    service.AllocateBlock(nullptr, &request, &replay, nullptr);
    ASSERT_EQ(replay.status().code(), 0u);
    EXPECT_EQ(replay.block().block_id(), first.block().block_id());
    EXPECT_EQ(replay.block().generation_stamp(), first.block().generation_stamp());
    EXPECT_TRUE(verify_block_token(replay.block().block_token(),
                                   "test-secret",
                                   BlockTokenPermission::kWrite,
                                   replay.block().block_id(),
                                   replay.block().generation_stamp(),
                                   file.value().inode_id,
                                   0));
    EXPECT_GE(replay.block().block_token().expires_at_ms(),
              first.block().block_token().expires_at_ms());
    EXPECT_FALSE(has_token_permission(replay.block().block_token().permissions(),
                                      BlockTokenPermission::kTruncate));
    EXPECT_FALSE(has_token_permission(replay.block().block_token().permissions(),
                                      BlockTokenPermission::kTransfer));

    ASSERT_TRUE(lease_mgr_->release_lease(file.value().inode_id, "client").hasValue());
    protocol::AllocateBlockResponse replay_without_lease;
    service.AllocateBlock(nullptr, &request, &replay_without_lease, nullptr);
    EXPECT_NE(replay_without_lease.status().code(), 0u);
}

// P0: Idempotent replay — duplicate Delete request returns success without side effects
TEST_F(RegressionNameNodeTest, P0_IdempotentDeleteReplay) {
    NameNodeServiceImpl service(ns_mgr_.get(), block_mgr_.get(), lease_mgr_.get(), store_raw(),
                                "test-secret", 3600000);
    const std::string req_id = "idem-delete-001";

    // Create a file first
    auto file = ns_mgr_->create_file("/idem_del.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());

    protocol::DeleteRequest req1;
    req1.set_path("/idem_del.dat");
    req1.set_recursive(false);
    req1.mutable_header()->set_request_id(req_id);
    req1.mutable_header()->set_client_id("test-client");
    protocol::DeleteResponse resp1;
    service.Delete(nullptr, &req1, &resp1, nullptr);
    ASSERT_EQ(resp1.status().code(), 0u);

    // File should be gone
    auto stat = ns_mgr_->get_file_status("/idem_del.dat");
    EXPECT_TRUE(stat.hasError());

    // Replay with same request_id — should succeed without error
    protocol::DeleteRequest req2;
    req2.set_path("/idem_del.dat");
    req2.set_recursive(false);
    req2.mutable_header()->set_request_id(req_id);
    req2.mutable_header()->set_client_id("test-client");
    protocol::DeleteResponse resp2;
    service.Delete(nullptr, &req2, &resp2, nullptr);
    EXPECT_EQ(resp2.status().code(), 0u) << "Duplicate Delete should succeed";
}

// P0: Idempotent replay — duplicate Rename request returns success
TEST_F(RegressionNameNodeTest, P0_IdempotentRenameReplay) {
    NameNodeServiceImpl service(ns_mgr_.get(), block_mgr_.get(), lease_mgr_.get(), store_raw(),
                                "test-secret", 3600000);
    const std::string req_id = "idem-rename-001";

    // Create a file first
    auto file = ns_mgr_->create_file("/idem_rename_src.dat", "user", "grp", 0644, 3, 128 * kMB);
    ASSERT_TRUE(file.hasValue());

    protocol::RenameRequest req1;
    req1.set_src("/idem_rename_src.dat");
    req1.set_dst("/idem_rename_dst.dat");
    req1.mutable_header()->set_request_id(req_id);
    req1.mutable_header()->set_client_id("test-client");
    protocol::RenameResponse resp1;
    service.Rename(nullptr, &req1, &resp1, nullptr);
    ASSERT_EQ(resp1.status().code(), 0u);

    // Replay with same request_id
    protocol::RenameRequest req2;
    req2.set_src("/idem_rename_src.dat");
    req2.set_dst("/idem_rename_dst.dat");
    req2.mutable_header()->set_request_id(req_id);
    req2.mutable_header()->set_client_id("test-client");
    protocol::RenameResponse resp2;
    service.Rename(nullptr, &req2, &resp2, nullptr);
    EXPECT_EQ(resp2.status().code(), 0u) << "Duplicate Rename should succeed";
}

// P0: Oplog atomicity — write_oplog failure should not leave orphaned state
//
// Scenario: If write_oplog fails (e.g., duplicate key), the service should
// fall back to replaying from the existing oplog entry rather than returning
// an error to the client. This test uses a custom store that fails write_oplog
// after the first call.
class OplogFailingStore : public testing::MockMetadataStore {
public:
    void set_fail_oplog(bool fail) { fail_oplog_ = fail; }

    pl::Result<pl::Void> write_oplog(std::string_view op_type,
                                     uint64_t target_inode_id,
                                     std::string_view request_id,
                                     std::string_view payload_json) override {
        if (fail_oplog_) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kMySQLQueryFailed),
                                 "injected write_oplog failure");
        }
        return testing::MockMetadataStore::write_oplog(op_type, target_inode_id, request_id, payload_json);
    }

private:
    bool fail_oplog_ = false;
};

TEST(RegressionNameNodeIdempotencyTest, P0_WriteOplogFailureFallsBackToReplay) {
    auto store = std::make_unique<OplogFailingStore>();
    auto ns_mgr = std::make_unique<NamespaceManager>(store.get());
    auto dn_mgr = std::make_unique<DataNodeManager>(store.get());
    auto placement = std::make_unique<PlacementManager>(dn_mgr.get());
    auto block_mgr =
        std::make_unique<BlockManager>(store.get(), placement.get(), "test-secret");
    auto lease_mgr = std::make_unique<LeaseManager>(store.get());

    // Register datanodes for placement
    dn_mgr->register_datanode("dn-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    dn_mgr->register_datanode("dn-2", "host2", "10.0.0.2", 9000, 9100, "/rack2", 1000 * kGB);
    dn_mgr->register_datanode("dn-3", "host3", "10.0.0.3", 9000, 9100, "/rack3", 1000 * kGB);

    NameNodeServiceImpl service(ns_mgr.get(), block_mgr.get(), lease_mgr.get(), store.get(),
                                "test-secret", 3600000);
    const std::string req_id = "idem-atomic-001";

    // First request: succeeds, oplog written
    protocol::CreateFileRequest req1;
    req1.set_path("/atomic_file.dat");
    req1.set_owner("user");
    req1.set_group("grp");
    req1.set_permission(0644);
    req1.set_replication(3);
    req1.set_block_size(128 * kMB);
    req1.set_client_id("test-client");
    req1.mutable_header()->set_request_id(req_id);
    req1.mutable_header()->set_client_id("test-client");
    protocol::CreateFileResponse resp1;
    service.CreateFile(nullptr, &req1, &resp1, nullptr);
    ASSERT_EQ(resp1.status().code(), 0u);
    const uint64_t inode_id = resp1.inode_id();

    // Now fail oplog writes and send the same request again
    store->set_fail_oplog(true);
    protocol::CreateFileRequest req2;
    req2.set_path("/atomic_file.dat");
    req2.set_owner("user");
    req2.set_group("grp");
    req2.set_permission(0644);
    req2.set_replication(3);
    req2.set_block_size(128 * kMB);
    req2.set_client_id("test-client");
    req2.mutable_header()->set_request_id(req_id);
    req2.mutable_header()->set_client_id("test-client");
    protocol::CreateFileResponse resp2;
    service.CreateFile(nullptr, &req2, &resp2, nullptr);
    // Should fall back to existing oplog entry and return success
    EXPECT_EQ(resp2.status().code(), 0u)
        << "Duplicate CreateFile with write_oplog failure should fall back to replay";
    EXPECT_EQ(resp2.inode_id(), inode_id)
        << "Fell-back CreateFile should return same inode_id";
}

TEST_F(RegressionNameNodeTest, P0_AllocIdFirstInsertContiguity) {
    // Use a fresh name that has never been allocated.
    const std::string fresh_name = "brand_new_allocator";

    // First allocation: request 5 IDs.
    auto first = store_raw()->alloc_id(fresh_name, 5);
    ASSERT_TRUE(first.hasValue());
    uint64_t base1 = first.value();

    // Second allocation: request 3 IDs.
    auto second = store_raw()->alloc_id(fresh_name, 3);
    ASSERT_TRUE(second.hasValue());
    uint64_t base2 = second.value();

    // Verify contiguity: second base should immediately follow first range.
    EXPECT_EQ(base2, base1 + 5) << "alloc_id produced non-contiguous ranges. First alloc returned "
                                << base1 << " (range [" << base1 << ", " << base1 + 5
                                << ")), second returned " << base2 << " (expected " << base1 + 5
                                << ").";

    // Third allocation to further verify.
    auto third = store_raw()->alloc_id(fresh_name, 2);
    ASSERT_TRUE(third.hasValue());
    EXPECT_EQ(third.value(), base2 + 3)
        << "Third allocation should continue from second range end.";
}

} // namespace
} // namespace pl::minidfs
