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

#include "absl/status/status.h"

namespace pl::minivessel {
namespace {

bool IsTerminal(ReplicaRole role) {
    return role == ReplicaRole::kTombstoned || role == ReplicaRole::kStopped;
}

bool IsAllowed(ReplicaRole from, ReplicaRole to) {
    if (from == to) {
        return true;
    }
    if (to == ReplicaRole::kFailed || to == ReplicaRole::kTombstoned ||
        to == ReplicaRole::kStopped) {
        return !IsTerminal(from);
    }
    if (to == ReplicaRole::kFenced) {
        return !IsTerminal(from) && from != ReplicaRole::kNew;
    }

    switch (from) {
        case ReplicaRole::kNew:
            return to == ReplicaRole::kRecovering;
        case ReplicaRole::kRecovering:
            return to == ReplicaRole::kStandbyCatchingUp;
        case ReplicaRole::kStandbyCatchingUp:
            return to == ReplicaRole::kStandbyReady;
        case ReplicaRole::kStandbyReady:
            return to == ReplicaRole::kStandbyCatchingUp || to == ReplicaRole::kPrimaryAcquiring;
        case ReplicaRole::kPrimaryAcquiring:
            return to == ReplicaRole::kPrimaryRecovering;
        case ReplicaRole::kPrimaryRecovering:
            return to == ReplicaRole::kPrimaryBarrier;
        case ReplicaRole::kPrimaryBarrier:
            return to == ReplicaRole::kPrimaryServing;
        case ReplicaRole::kPrimaryServing:
            return to == ReplicaRole::kPrimaryDraining;
        case ReplicaRole::kPrimaryDraining:
            return to == ReplicaRole::kStandbyCatchingUp;
        case ReplicaRole::kFenced:
            return to == ReplicaRole::kStandbyCatchingUp || to == ReplicaRole::kRecovering;
        case ReplicaRole::kFailed:
            return to == ReplicaRole::kRecovering;
        case ReplicaRole::kTombstoned:
        case ReplicaRole::kStopped:
            return false;
    }
    return false;
}

} // namespace

bool RoleStateMachine::can_tail() const {
    switch (role_) {
        case ReplicaRole::kRecovering:
        case ReplicaRole::kStandbyCatchingUp:
        case ReplicaRole::kStandbyReady:
        case ReplicaRole::kPrimaryAcquiring:
        case ReplicaRole::kPrimaryRecovering:
        case ReplicaRole::kPrimaryBarrier:
        case ReplicaRole::kPrimaryServing:
        case ReplicaRole::kPrimaryDraining:
        case ReplicaRole::kFenced:
            return true;
        case ReplicaRole::kNew:
        case ReplicaRole::kFailed:
        case ReplicaRole::kTombstoned:
        case ReplicaRole::kStopped:
            return false;
    }
    return false;
}

absl::Status RoleStateMachine::transition_to(ReplicaRole next) {
    if (!IsAllowed(role_, next)) {
        return absl::FailedPreconditionError("invalid MiniVessel role transition");
    }
    role_ = next;
    return absl::OkStatus();
}

} // namespace pl::minivessel
