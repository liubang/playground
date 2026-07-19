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

#include "cpp/pl/minivessel/role_state.h"
#include "gtest/gtest.h"

namespace pl::minivessel {
namespace {

TEST(RoleStateMachineTest, PromotionRequiresRecoveryAndBarrier) {
    RoleStateMachine state;

    EXPECT_FALSE(state.can_accept_writes());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kRecovering).ok());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kStandbyCatchingUp).ok());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kStandbyReady).ok());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kPrimaryAcquiring).ok());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kPrimaryRecovering).ok());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kPrimaryBarrier).ok());
    EXPECT_FALSE(state.can_accept_writes());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kPrimaryServing).ok());
    EXPECT_TRUE(state.can_accept_writes());
}

TEST(RoleStateMachineTest, CannotSkipPrimaryBarrier) {
    RoleStateMachine state;
    ASSERT_TRUE(state.transition_to(ReplicaRole::kRecovering).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kStandbyCatchingUp).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kStandbyReady).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kPrimaryAcquiring).ok());

    EXPECT_FALSE(state.transition_to(ReplicaRole::kPrimaryServing).ok());
    EXPECT_EQ(state.role(), ReplicaRole::kPrimaryAcquiring);
}

TEST(RoleStateMachineTest, FencingClosesWriteAdmission) {
    RoleStateMachine state;
    ASSERT_TRUE(state.transition_to(ReplicaRole::kRecovering).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kStandbyCatchingUp).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kStandbyReady).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kPrimaryAcquiring).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kPrimaryRecovering).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kPrimaryBarrier).ok());
    ASSERT_TRUE(state.transition_to(ReplicaRole::kPrimaryServing).ok());

    ASSERT_TRUE(state.transition_to(ReplicaRole::kFenced).ok());
    EXPECT_FALSE(state.can_accept_writes());
    EXPECT_TRUE(state.can_tail());
    EXPECT_TRUE(state.transition_to(ReplicaRole::kStandbyCatchingUp).ok());
}

TEST(RoleStateMachineTest, StoppedReplicaCannotRestartInPlace) {
    RoleStateMachine state;
    ASSERT_TRUE(state.transition_to(ReplicaRole::kStopped).ok());

    EXPECT_FALSE(state.transition_to(ReplicaRole::kRecovering).ok());
    EXPECT_EQ(state.role(), ReplicaRole::kStopped);
}

} // namespace
} // namespace pl::minivessel
