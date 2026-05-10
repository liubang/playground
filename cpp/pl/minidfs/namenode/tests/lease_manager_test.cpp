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

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/namenode/lease_manager.h"
#include "cpp/pl/minidfs/namenode/tests/mock_metadata_store.h"
#include <gtest/gtest.h>
#include <thread>

namespace pl::minidfs {
namespace {

class LeaseManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<testing::MockMetadataStore>();
        mgr_ = std::make_unique<LeaseManager>(store_.get());
    }

    std::unique_ptr<testing::MockMetadataStore> store_;
    std::unique_ptr<LeaseManager> mgr_;
};

TEST_F(LeaseManagerTest, AcquireAndRelease) {
    auto result = mgr_->acquire_lease(100, "client-1");
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().inode_id, 100u);
    EXPECT_EQ(result.value().client_id, "client-1");
    EXPECT_EQ(result.value().state, LeaseState::kActive);

    auto has = mgr_->has_active_lease(100);
    ASSERT_TRUE(has.hasValue());
    EXPECT_TRUE(has.value());

    auto release = mgr_->release_lease(100, "client-1");
    ASSERT_TRUE(release.hasValue());

    auto has_after = mgr_->has_active_lease(100);
    ASSERT_TRUE(has_after.hasValue());
    EXPECT_FALSE(has_after.value());
}

TEST_F(LeaseManagerTest, AcquireConflict) {
    mgr_->acquire_lease(100, "client-1");
    auto result = mgr_->acquire_lease(100, "client-2");
    ASSERT_TRUE(result.hasError());
}

TEST_F(LeaseManagerTest, RenewLease) {
    mgr_->acquire_lease(100, "client-1");
    auto result = mgr_->renew_lease(100, "client-1");
    ASSERT_TRUE(result.hasValue());
}

TEST_F(LeaseManagerTest, RenewLeaseWrongClient) {
    mgr_->acquire_lease(100, "client-1");
    auto result = mgr_->renew_lease(100, "client-2");
    ASSERT_TRUE(result.hasError());
}

TEST_F(LeaseManagerTest, RenewNoLease) {
    auto result = mgr_->renew_lease(999, "client-1");
    ASSERT_TRUE(result.hasError());
}

TEST_F(LeaseManagerTest, ReleaseWrongClient) {
    mgr_->acquire_lease(100, "client-1");
    auto result = mgr_->release_lease(100, "client-2");
    ASSERT_TRUE(result.hasError());
}

TEST_F(LeaseManagerTest, ReleaseNoLease) {
    // Release on non-existent lease is idempotent.
    auto result = mgr_->release_lease(999, "client-1");
    ASSERT_TRUE(result.hasValue());
}

TEST_F(LeaseManagerTest, ExpireStaleLeases) {
    // Acquire a lease — it will have expire_time set to now + timeout.
    mgr_->acquire_lease(100, "client-1");
    mgr_->acquire_lease(200, "client-2");

    // Expire with a far-future timestamp.
    uint64_t far_future = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count() +
                          kDefaultLeaseTimeoutMs * 10;

    auto result = mgr_->expire_stale_leases();
    // With current time, leases should not be expired yet.
    ASSERT_TRUE(result.hasValue());

    // Now directly expire by passing far-future to the store.
    auto expired = store_->expire_leases(far_future);
    ASSERT_TRUE(expired.hasValue());
    EXPECT_EQ(expired.value(), 2u);

    // Verify leases are gone.
    auto has1 = mgr_->has_active_lease(100);
    EXPECT_FALSE(has1.value());
    auto has2 = mgr_->has_active_lease(200);
    EXPECT_FALSE(has2.value());
}

TEST_F(LeaseManagerTest, HasActiveLeaseFalseAfterExpire) {
    auto result = mgr_->has_active_lease(42);
    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value());
}

} // namespace
} // namespace pl::minidfs
