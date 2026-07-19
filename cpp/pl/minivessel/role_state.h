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

#pragma once

#include <cstdint>

#include "absl/status/status.h"

namespace pl::minivessel {

enum class ReplicaRole : uint8_t {
    kNew,
    kRecovering,
    kStandbyCatchingUp,
    kStandbyReady,
    kPrimaryAcquiring,
    kPrimaryRecovering,
    kPrimaryBarrier,
    kPrimaryServing,
    kPrimaryDraining,
    kFenced,
    kFailed,
    kTombstoned,
    kStopped,
};

class RoleStateMachine final {
public:
    [[nodiscard]] ReplicaRole role() const { return role_; }
    [[nodiscard]] bool can_accept_writes() const { return role_ == ReplicaRole::kPrimaryServing; }
    [[nodiscard]] bool can_tail() const;

    [[nodiscard]] absl::Status transition_to(ReplicaRole next);

private:
    ReplicaRole role_ = ReplicaRole::kNew;
};

} // namespace pl::minivessel
