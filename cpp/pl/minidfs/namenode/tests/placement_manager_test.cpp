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
#include <set>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"

namespace pl::minidfs {
namespace {

class PlacementManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        dn_mgr_ = std::make_unique<DataNodeManager>(store_.get());
        placement_ = std::make_unique<PlacementManager>(dn_mgr_.get());

        // Register 4 datanodes across 2 racks.
        dn_mgr_->register_datanode("dn-1", "host1", "10.0.0.1", 9000, 9100, "/rack1", 1000 * kGB);
        dn_mgr_->register_datanode("dn-2", "host2", "10.0.0.2", 9000, 9100, "/rack1", 800 * kGB);
        dn_mgr_->register_datanode("dn-3", "host3", "10.0.0.3", 9000, 9100, "/rack2", 1200 * kGB);
        dn_mgr_->register_datanode("dn-4", "host4", "10.0.0.4", 9000, 9100, "/rack2", 900 * kGB);
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<DataNodeManager> dn_mgr_;
    std::unique_ptr<PlacementManager> placement_;
};

TEST_F(PlacementManagerTest, ChooseTargetsBasic) {
    auto result = placement_->choose_targets(3, std::nullopt);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 3u);

    // All chosen should be distinct.
    std::set<uint64_t> ids;
    for (const auto& dn : result.value()) {
        ids.insert(dn.datanode_id);
    }
    EXPECT_EQ(ids.size(), 3u);
}

TEST_F(PlacementManagerTest, ChooseTargetsWithExclude) {
    // Get first datanode's ID.
    auto all = dn_mgr_->get_live_datanodes();
    ASSERT_TRUE(all.hasValue());
    uint64_t exclude_id = all.value()[0].datanode_id;

    auto result = placement_->choose_targets(3, exclude_id);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 3u);

    for (const auto& dn : result.value()) {
        EXPECT_NE(dn.datanode_id, exclude_id);
    }
}

TEST_F(PlacementManagerTest, NotEnoughNodes) {
    auto result = placement_->choose_targets(5, std::nullopt);
    ASSERT_TRUE(result.hasError());
}

TEST_F(PlacementManagerTest, ChooseSingleReplica) {
    auto result = placement_->choose_targets(1, std::nullopt);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().size(), 1u);
}

TEST_F(PlacementManagerTest, RackAwareness) {
    // With 4 nodes across 2 racks, choosing 2 replicas should ideally
    // pick from different racks. Run multiple times to verify the tendency.
    int cross_rack_count = 0;
    const int trials = 20;

    for (int i = 0; i < trials; ++i) {
        auto result = placement_->choose_targets(2, std::nullopt);
        ASSERT_TRUE(result.hasValue());
        ASSERT_EQ(result.value().size(), 2u);
        if (result.value()[0].rack != result.value()[1].rack) {
            ++cross_rack_count;
        }
    }

    // With rack-aware placement, most trials should pick cross-rack.
    // We allow some tolerance since there's randomness involved.
    EXPECT_GT(cross_rack_count, trials / 2);
}

} // namespace
} // namespace pl::minidfs
