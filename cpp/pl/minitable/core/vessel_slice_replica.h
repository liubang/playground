// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "cpp/pl/minitable/core/vessel_slice_state_machine.h"
#include "cpp/pl/minivessel/replica_runtime.h"

namespace pl::minitable {

// One Slice replica hosted by a UnitServer. The control plane explicitly promotes/demotes it;
// MiniVessel provides WAL fencing, catch-up, Primary barrier, and Standby tail replay.
class VesselSliceReplica final {
public:
    [[nodiscard]] static absl::StatusOr<std::unique_ptr<VesselSliceReplica>> Create(
        minivessel::SharedWal* wal,
        std::unique_ptr<VesselSliceStateMachine> state_machine,
        minivessel::ReplicaRuntimeOptions options);
    ~VesselSliceReplica();

    VesselSliceReplica(const VesselSliceReplica&) = delete;
    VesselSliceReplica& operator=(const VesselSliceReplica&) = delete;

    [[nodiscard]] absl::Status poll();
    [[nodiscard]] absl::Status promote();
    [[nodiscard]] absl::Status demote();
    [[nodiscard]] absl::StatusOr<minivessel::CommitResult> submit(
        std::string request_id, std::span<const std::byte> encoded_mutation);
    void stop();

    [[nodiscard]] minivessel::ReplicaStatus status() const;
    [[nodiscard]] const VesselSliceStateMachine& state_machine() const noexcept {
        return *state_machine_;
    }

private:
    VesselSliceReplica(minivessel::SharedWal* wal,
                       std::unique_ptr<VesselSliceStateMachine> state_machine,
                       minivessel::ReplicaRuntimeOptions options);

    std::unique_ptr<VesselSliceStateMachine> state_machine_;
    std::unique_ptr<minivessel::ReplicaRuntime> runtime_;
    // Runtime role callbacks are explicitly reentrant. A recursive mutex prevents callback-driven
    // status/control code on the same thread from deadlocking while still serializing sessions.
    mutable std::recursive_mutex session_mutex_;
    std::optional<minivessel::ReplicaRuntime::PrimarySession> primary_session_;
};

} // namespace pl::minitable
