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
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

class DataNodeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        mgr_ = std::make_unique<DataNodeManager>(store_.get());
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<DataNodeManager> mgr_;
};

TEST_F(DataNodeManagerTest, RegisterNew) {
    auto result =
        mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    ASSERT_TRUE(result.hasValue());
    EXPECT_GT(result.value(), 0u);

    // Verify we can retrieve it.
    auto dn = mgr_->get_datanode(result.value());
    ASSERT_TRUE(dn.hasValue());
    EXPECT_EQ(dn.value().uuid, "uuid-1");
    EXPECT_EQ(dn.value().hostname, "host1");
    EXPECT_EQ(dn.value().state, DataNodeState::kLive);
}

TEST_F(DataNodeManagerTest, ReRegister) {
    auto first =
        mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    ASSERT_TRUE(first.hasValue());

    // Re-register with different hostname.
    auto second = mgr_->register_datanode(
        "uuid-1", "host1-new", "10.0.0.2", 9000, 9100, "/rack2", 2000 * kGB);
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value(), second.value());

    auto dn = mgr_->get_datanode(first.value());
    EXPECT_EQ(dn.value().hostname, "host1-new");
    EXPECT_EQ(dn.value().rack, "/rack2");
}

TEST_F(DataNodeManagerTest, Heartbeat) {
    auto id =
        mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    ASSERT_TRUE(id.hasValue());

    auto result = mgr_->handle_heartbeat(id.value(), 1000 * kGB, 200 * kGB, 800 * kGB);
    ASSERT_TRUE(result.hasValue());

    auto dn = mgr_->get_datanode(id.value());
    EXPECT_EQ(dn.value().used_bytes, 200 * kGB);
    EXPECT_EQ(dn.value().free_bytes, 800 * kGB);
}

TEST_F(DataNodeManagerTest, HeartbeatUnknownNode) {
    auto result = mgr_->handle_heartbeat(9999, 100, 50, 50);
    ASSERT_TRUE(result.hasError());
}

TEST_F(DataNodeManagerTest, CheckStaleAndDead) {
    auto id =
        mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    ASSERT_TRUE(id.hasValue());

    // Manually set last_heartbeat_ms to past to simulate stale.
    auto dn = store_->get_datanode(id.value());
    ASSERT_TRUE(dn.hasValue());
    auto node = dn.value();
    node.last_heartbeat_ms = 1; // Very old timestamp.
    store_->upsert_datanode(node);

    auto changed = mgr_->check_stale_and_dead();
    ASSERT_TRUE(changed.hasValue());
    EXPECT_EQ(changed.value(), 1u);

    auto updated = mgr_->get_datanode(id.value());
    // Should be Dead since timestamp is ancient.
    EXPECT_EQ(updated.value().state, DataNodeState::kDead);
}

TEST_F(DataNodeManagerTest, GetLiveDatanodes) {
    mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
    mgr_->register_datanode("uuid-2", "host2", "10.0.0.2", 9000, 9100, "/rack2", 1000 * kGB);

    auto live = mgr_->get_live_datanodes();
    ASSERT_TRUE(live.hasValue());
    EXPECT_EQ(live.value().size(), 2u);
}

TEST_F(DataNodeManagerTest, GetAllDatanodes) {
    mgr_->register_datanode("uuid-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);

    auto all = mgr_->get_all_datanodes();
    ASSERT_TRUE(all.hasValue());
    EXPECT_EQ(all.value().size(), 1u);
}

} // namespace
} // namespace pl::minidfs
