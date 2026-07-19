// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/vessel_slice_replica.h"

#include <utility>

#include "absl/status/status.h"

namespace pl::minitable {

absl::StatusOr<std::unique_ptr<VesselSliceReplica>> VesselSliceReplica::Create(
    minivessel::SharedWal* wal,
    std::unique_ptr<VesselSliceStateMachine> state_machine,
    minivessel::ReplicaRuntimeOptions options) {
    if (wal == nullptr || state_machine == nullptr) {
        return absl::InvalidArgumentError("Vessel Slice replica requires WAL and state machine");
    }
    if (options.replica_id.empty() || !options.assignment_epoch.valid()) {
        return absl::InvalidArgumentError("Vessel Slice replica requires a valid assignment");
    }
    return std::unique_ptr<VesselSliceReplica>(
        new VesselSliceReplica(wal, std::move(state_machine), std::move(options)));
}

VesselSliceReplica::VesselSliceReplica(
    minivessel::SharedWal* wal,
    std::unique_ptr<VesselSliceStateMachine> state_machine,
    minivessel::ReplicaRuntimeOptions options)
    : state_machine_(std::move(state_machine)),
      runtime_(std::make_unique<minivessel::ReplicaRuntime>(wal, state_machine_.get(),
                                                            std::move(options))) {}

VesselSliceReplica::~VesselSliceReplica() { stop(); }

absl::Status VesselSliceReplica::poll() { return runtime_->poll(); }

absl::Status VesselSliceReplica::promote() {
    std::lock_guard lock(session_mutex_);
    if (primary_session_.has_value()) {
        const auto guard = runtime_->read(primary_session_->token());
        if (guard.token_valid()) {
            return absl::AlreadyExistsError("Slice replica is already Primary");
        }
        primary_session_.reset();
    }
    auto session = runtime_->promote_session();
    if (!session.ok()) {
        return session.status();
    }
    primary_session_.emplace(std::move(*session));
    return absl::OkStatus();
}

absl::Status VesselSliceReplica::demote() {
    std::lock_guard lock(session_mutex_);
    if (!primary_session_.has_value()) {
        return absl::OkStatus();
    }
    auto status = primary_session_->demote();
    primary_session_.reset();
    return status;
}

absl::StatusOr<minivessel::CommitResult> VesselSliceReplica::submit(
    std::string request_id, std::span<const std::byte> encoded_mutation) {
    std::lock_guard lock(session_mutex_);
    if (!primary_session_.has_value() || !primary_session_->valid()) {
        return absl::FailedPreconditionError("Slice replica is not Primary");
    }
    if (request_id.empty() || encoded_mutation.empty()) {
        return absl::InvalidArgumentError("Slice mutation requires request ID and payload");
    }
    auto result = primary_session_->submit(std::move(request_id), encoded_mutation);
    if (!result.ok() && runtime_->status().role != minivessel::RuntimeRole::kPrimary) {
        primary_session_.reset();
    }
    return result;
}

void VesselSliceReplica::stop() {
    std::lock_guard lock(session_mutex_);
    primary_session_.reset();
    if (runtime_ != nullptr) {
        runtime_->stop();
    }
}

minivessel::ReplicaStatus VesselSliceReplica::status() const { return runtime_->status(); }

} // namespace pl::minitable
