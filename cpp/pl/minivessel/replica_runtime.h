// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/shared_wal.h"

namespace pl::minivessel {

enum class ApplySource : uint8_t {
    kPrimaryCommit,
    kWalReplay,
};

struct ApplyContext final {
    ApplySource source = ApplySource::kWalReplay;
    WriterEpoch writer_epoch;

    [[nodiscard]] bool is_primary_commit() const noexcept {
        return source == ApplySource::kPrimaryCommit;
    }
};

class ApplyResult final {
public:
    [[nodiscard]] static ApplyResult Applied() { return ApplyResult(true, absl::OkStatus()); }
    [[nodiscard]] static ApplyResult Rejected(absl::Status reason) {
        return ApplyResult(false, std::move(reason));
    }

    [[nodiscard]] bool applied() const noexcept { return applied_; }
    [[nodiscard]] bool rejected() const noexcept { return !applied_; }
    [[nodiscard]] const absl::Status& rejection() const noexcept { return rejection_; }

private:
    ApplyResult(bool applied, absl::Status rejection)
        : applied_(applied), rejection_(std::move(rejection)) {}

    bool applied_ = true;
    absl::Status rejection_;
};

class ReplicatedStateMachine {
public:
    virtual ~ReplicatedStateMachine() = default;
    // A non-OK StatusOr is a system failure. ApplyResult::Rejected is a deterministic business
    // outcome: it advances applied LRSN and must be reproduced during replay.
    virtual absl::StatusOr<ApplyResult> apply(const LogRecord& record,
                                              const ApplyContext& context) = 0;
    virtual absl::StatusOr<std::vector<std::byte>> create_checkpoint() {
        return absl::UnimplementedError("state machine does not support checkpoints");
    }
    virtual absl::Status restore_checkpoint(std::span<const std::byte>, Lrsn) {
        return absl::UnimplementedError("state machine does not support checkpoints");
    }
};

enum class RuntimeRole : uint8_t {
    kStandby,
    kPromoting,
    kPrimary,
    kFailed,
    kStopped,
};

struct ReplicaStatus final {
    RuntimeRole role = RuntimeRole::kStandby;
    WriterEpoch writer_epoch;
    Lrsn applied_lrsn;
    absl::Status last_error;
};

class RoleLifecycle {
public:
    virtual ~RoleLifecycle() = default;
    // Invoked without the runtime mutex held. Implementations may safely call runtime APIs.
    virtual void on_role_change(RuntimeRole previous, const ReplicaStatus& current) = 0;
};

struct ReplicaRuntimeOptions final {
    std::string replica_id;
    AssignmentEpoch assignment_epoch;
    uint64_t writer_lease_timeout_ms = 30'000;
    size_t tail_batch_records = 128;
    RoleLifecycle* role_lifecycle = nullptr;
};

struct PrimaryToken final {
    uint64_t runtime_generation = 0;
    WriterEpoch writer_epoch;

    [[nodiscard]] bool valid() const noexcept {
        return runtime_generation != 0 && writer_epoch.valid();
    }
    friend bool operator==(const PrimaryToken&, const PrimaryToken&) = default;
};

struct CommitResult final {
    Lrsn lrsn;
    ApplyResult apply_result = ApplyResult::Applied();
};

class ReplicaRuntime final {
public:
    class PrimarySession;
    class ReadGuard;

    ReplicaRuntime(SharedWal* wal,
                   ReplicatedStateMachine* state_machine,
                   ReplicaRuntimeOptions options);
    ~ReplicaRuntime();

    ReplicaRuntime(const ReplicaRuntime&) = delete;
    ReplicaRuntime& operator=(const ReplicaRuntime&) = delete;

    absl::Status poll();
    absl::StatusOr<PrimarySession> promote_session();
    absl::StatusOr<CommitResult> submit(const PrimaryToken& token,
                                        std::string request_id,
                                        std::span<const std::byte> payload);
    absl::StatusOr<CommitResult> checkpoint(const PrimaryToken& token);
    absl::Status demote(const PrimaryToken& token);
    void stop();
    [[nodiscard]] ReplicaStatus status() const;
    [[nodiscard]] ReadGuard read(PrimaryToken token = {});

private:
    struct LifetimeControl final {
        std::mutex mutex;
        ReplicaRuntime* runtime = nullptr;
    };

    struct RoleChange final {
        RuntimeRole previous;
        ReplicaStatus current;
    };

    absl::Status catch_up_locked();
    absl::Status renew_writer_locked();
    absl::StatusOr<ApplyResult> apply_locked(const LogRecord& record, ApplySource source);
    absl::Status fail_locked(absl::Status status);
    void set_role_locked(RuntimeRole role);
    [[nodiscard]] bool token_valid_locked(const PrimaryToken& token) const noexcept;
    [[nodiscard]] std::vector<RoleChange> take_role_changes_locked();
    void notify_role_changes();
    absl::Status demote_locked(const PrimaryToken* token);
    absl::Status release_writer_locked();

    SharedWal* wal_;
    ReplicatedStateMachine* state_machine_;
    ReplicaRuntimeOptions options_;
    mutable std::mutex mutex_;
    ReplicaStatus status_;
    uint64_t generation_ = 0;
    bool writer_held_ = false;
    std::vector<RoleChange> role_changes_;
    // Protected by mutex_. Exactly one caller drains callbacks; reentrant/concurrent transitions
    // only enqueue and are picked up by that dispatcher in transition order.
    bool notifying_role_changes_ = false;
    std::shared_ptr<LifetimeControl> lifetime_;
};

class ReplicaRuntime::PrimarySession final {
public:
    PrimarySession() = default;
    PrimarySession(const PrimarySession&) = delete;
    PrimarySession& operator=(const PrimarySession&) = delete;
    PrimarySession(PrimarySession&& other) noexcept;
    PrimarySession& operator=(PrimarySession&& other) noexcept;
    ~PrimarySession();

    [[nodiscard]] const PrimaryToken& token() const noexcept { return token_; }
    [[nodiscard]] bool valid() const noexcept { return !lifetime_.expired() && token_.valid(); }
    absl::StatusOr<CommitResult> submit(std::string request_id, std::span<const std::byte> payload);
    absl::StatusOr<CommitResult> checkpoint();
    absl::Status demote();

private:
    friend class ReplicaRuntime;
    PrimarySession(std::weak_ptr<LifetimeControl> lifetime, PrimaryToken token)
        : lifetime_(std::move(lifetime)), token_(token) {}

    std::weak_ptr<LifetimeControl> lifetime_;
    PrimaryToken token_;
};

class ReplicaRuntime::ReadGuard final {
public:
    ReadGuard(ReadGuard&&) noexcept = default;
    ReadGuard& operator=(ReadGuard&&) noexcept = default;
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

    [[nodiscard]] const ReplicaStatus& status() const noexcept { return status_; }
    [[nodiscard]] bool token_valid() const noexcept { return token_valid_; }
    [[nodiscard]] const ReplicatedStateMachine& state_machine() const noexcept {
        return *state_machine_;
    }

private:
    friend class ReplicaRuntime;
    ReadGuard(std::unique_lock<std::mutex> lock,
              ReplicaStatus status,
              bool token_valid,
              ReplicatedStateMachine* state_machine)
        : lock_(std::move(lock)),
          status_(std::move(status)),
          token_valid_(token_valid),
          state_machine_(state_machine) {}

    std::unique_lock<std::mutex> lock_;
    ReplicaStatus status_;
    bool token_valid_ = false;
    ReplicatedStateMachine* state_machine_ = nullptr;
};

} // namespace pl::minivessel
