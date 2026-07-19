// Copyright (c) 2026 The Authors. All rights reserved.

#include "cpp/pl/minivessel/replica_runtime.h"

#include <limits>
#include <utility>

namespace pl::minivessel {

ReplicaRuntime::ReplicaRuntime(SharedWal* wal,
                               ReplicatedStateMachine* state_machine,
                               ReplicaRuntimeOptions options)
    : wal_(wal),
      state_machine_(state_machine),
      options_(std::move(options)),
      lifetime_(std::make_shared<LifetimeControl>()) {
    lifetime_->runtime = this;
}

ReplicaRuntime::~ReplicaRuntime() {
    std::lock_guard lifetime_lock(lifetime_->mutex);
    stop();
    lifetime_->runtime = nullptr;
}

void ReplicaRuntime::set_role_locked(RuntimeRole role) {
    if (status_.role == role) {
        return;
    }
    const RuntimeRole previous = status_.role;
    status_.role = role;
    role_changes_.push_back(RoleChange{previous, status_});
}

std::vector<ReplicaRuntime::RoleChange> ReplicaRuntime::take_role_changes_locked() {
    return std::exchange(role_changes_, {});
}

void ReplicaRuntime::notify_role_changes() {
    if (options_.role_lifecycle == nullptr) {
        std::lock_guard lock(mutex_);
        role_changes_.clear();
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (notifying_role_changes_ || role_changes_.empty()) {
            return;
        }
        notifying_role_changes_ = true;
    }
    while (true) {
        std::vector<RoleChange> changes;
        {
            std::lock_guard lock(mutex_);
            changes = take_role_changes_locked();
            if (changes.empty()) {
                notifying_role_changes_ = false;
                return;
            }
        }
        for (const auto& change : changes) {
            options_.role_lifecycle->on_role_change(change.previous, change.current);
        }
    }
}

absl::Status ReplicaRuntime::release_writer_locked() {
    if (!writer_held_) {
        return absl::OkStatus();
    }
    writer_held_ = false;
    status_.writer_epoch = {};
    return wal_->release_writer();
}

absl::Status ReplicaRuntime::fail_locked(absl::Status status) {
    status_.last_error = status;
    (void)release_writer_locked();
    set_role_locked(RuntimeRole::kFailed);
    return status;
}

absl::StatusOr<ApplyResult> ReplicaRuntime::apply_locked(const LogRecord& record,
                                                         ApplySource source) {
    if (status_.applied_lrsn.value() == std::numeric_limits<uint64_t>::max()) {
        return fail_locked(absl::OutOfRangeError("replica exhausted the LRSN space"));
    }
    if (record.lrsn.value() != status_.applied_lrsn.value() + 1) {
        return fail_locked(absl::DataLossError("replica observed a WAL LRSN gap or duplicate"));
    }

    ApplyResult result = ApplyResult::Applied();
    switch (record.type) {
        case LogRecordType::kMutation: {
            auto applied = state_machine_->apply(
                record, ApplyContext{.source = source, .writer_epoch = record.writer_epoch});
            if (!applied.ok()) {
                return fail_locked(absl::InternalError("replicated state machine system failure: " +
                                                       applied.status().ToString()));
            }
            result = std::move(*applied);
            break;
        }
        case LogRecordType::kCheckpoint: {
            auto restored = state_machine_->restore_checkpoint(record.payload, record.lrsn);
            if (!restored.ok()) {
                return fail_locked(absl::InternalError("replicated checkpoint restore failed: " +
                                                       restored.ToString()));
            }
            break;
        }
        case LogRecordType::kPrimaryBarrier:
            break;
    }
    // Deterministic business rejection is itself the applied outcome of this durable record.
    status_.applied_lrsn = record.lrsn;
    return result;
}

absl::Status ReplicaRuntime::catch_up_locked() {
    while (true) {
        auto records =
            wal_->read(Lrsn(status_.applied_lrsn.value() + 1), options_.tail_batch_records);
        if (!records.ok()) {
            if (records.status().code() == absl::StatusCode::kUnavailable) {
                return records.status();
            }
            return fail_locked(records.status());
        }
        if (records->empty()) {
            return absl::OkStatus();
        }
        for (const auto& record : *records) {
            auto result = apply_locked(record, ApplySource::kWalReplay);
            if (!result.ok()) {
                return result.status();
            }
        }
        if (records->size() < options_.tail_batch_records) {
            return absl::OkStatus();
        }
    }
}

absl::Status ReplicaRuntime::renew_writer_locked() {
    auto lease = wal_->renew_writer();
    if (!lease.ok() || lease->writer_epoch != status_.writer_epoch) {
        const absl::Status error =
            lease.ok() ? absl::AbortedError("writer epoch changed while renewing") : lease.status();
        (void)release_writer_locked();
        set_role_locked(RuntimeRole::kStandby);
        return error;
    }
    return absl::OkStatus();
}

absl::Status ReplicaRuntime::poll() {
    absl::Status result;
    {
        std::lock_guard lock(mutex_);
        if (status_.role == RuntimeRole::kFailed) {
            result = status_.last_error;
        } else if (status_.role == RuntimeRole::kStopped) {
            result = absl::FailedPreconditionError("replica is stopped");
        } else {
            result =
                status_.role == RuntimeRole::kPrimary ? renew_writer_locked() : absl::OkStatus();
            if (result.ok()) {
                result = catch_up_locked();
            }
        }
    }
    notify_role_changes();
    return result;
}

absl::StatusOr<ReplicaRuntime::PrimarySession> ReplicaRuntime::promote_session() {
    if (wal_ == nullptr || state_machine_ == nullptr || options_.replica_id.empty() ||
        !options_.assignment_epoch.valid() || options_.writer_lease_timeout_ms == 0 ||
        options_.tail_batch_records == 0) {
        return absl::InvalidArgumentError("promotion requires WAL, state machine, replica ID, "
                                          "assignment epoch, lease timeout and batch size");
    }
    absl::Status result;
    PrimaryToken token;
    {
        std::lock_guard lock(mutex_);
        if (status_.role == RuntimeRole::kFailed) {
            result = status_.last_error;
        } else if (status_.role != RuntimeRole::kStandby) {
            result = absl::FailedPreconditionError("only a standby can be promoted");
        } else {
            set_role_locked(RuntimeRole::kPromoting);
            auto lease = wal_->acquire_writer(
                options_.replica_id, options_.assignment_epoch, options_.writer_lease_timeout_ms);
            if (!lease.ok()) {
                set_role_locked(RuntimeRole::kStandby);
                result = lease.status();
            } else {
                status_.writer_epoch = lease->writer_epoch;
                writer_held_ = true;
                result = catch_up_locked();
                if (result.ok()) {
                    auto barrier = wal_->append(LogRecordType::kPrimaryBarrier, {}, {});
                    if (!barrier.ok()) {
                        result = barrier.status();
                    } else {
                        auto applied = apply_locked(barrier->record, ApplySource::kPrimaryCommit);
                        result = applied.ok() ? absl::OkStatus() : applied.status();
                    }
                }
                if (result.ok()) {
                    ++generation_;
                    if (generation_ == 0) {
                        result = fail_locked(absl::OutOfRangeError("runtime generation exhausted"));
                    } else {
                        set_role_locked(RuntimeRole::kPrimary);
                        token = PrimaryToken{generation_, status_.writer_epoch};
                    }
                } else {
                    (void)release_writer_locked();
                    if (status_.role != RuntimeRole::kFailed) {
                        set_role_locked(RuntimeRole::kStandby);
                    }
                }
            }
        }
    }
    notify_role_changes();
    if (!result.ok()) {
        return result;
    }
    return PrimarySession(lifetime_, token);
}

bool ReplicaRuntime::token_valid_locked(const PrimaryToken& token) const noexcept {
    return status_.role == RuntimeRole::kPrimary && token.valid() &&
           token.runtime_generation == generation_ && token.writer_epoch == status_.writer_epoch;
}

absl::StatusOr<CommitResult> ReplicaRuntime::submit(const PrimaryToken& token,
                                                    std::string request_id,
                                                    std::span<const std::byte> payload) {
    absl::StatusOr<CommitResult> result =
        absl::FailedPreconditionError("stale or invalid primary token");
    {
        std::lock_guard lock(mutex_);
        if (token_valid_locked(token)) {
            auto durable = wal_->append(LogRecordType::kMutation, std::move(request_id), payload);
            if (!durable.ok()) {
                (void)release_writer_locked();
                set_role_locked(RuntimeRole::kStandby);
                result = durable.status();
            } else {
                auto applied = apply_locked(durable->record, ApplySource::kPrimaryCommit);
                result = applied.ok() ? absl::StatusOr<CommitResult>(
                                            CommitResult{durable->record.lrsn, std::move(*applied)})
                                      : absl::StatusOr<CommitResult>(applied.status());
            }
        }
    }
    notify_role_changes();
    return result;
}

absl::StatusOr<CommitResult> ReplicaRuntime::checkpoint(const PrimaryToken& token) {
    absl::StatusOr<CommitResult> result =
        absl::FailedPreconditionError("stale or invalid primary token");
    {
        std::lock_guard lock(mutex_);
        if (token_valid_locked(token)) {
            auto payload = state_machine_->create_checkpoint();
            if (!payload.ok()) {
                result = payload.status();
            } else {
                auto durable = wal_->append(LogRecordType::kCheckpoint, {}, *payload);
                if (!durable.ok()) {
                    (void)release_writer_locked();
                    set_role_locked(RuntimeRole::kStandby);
                    result = durable.status();
                } else {
                    auto applied = apply_locked(durable->record, ApplySource::kPrimaryCommit);
                    result = applied.ok() ? absl::StatusOr<CommitResult>(CommitResult{
                                                durable->record.lrsn, std::move(*applied)})
                                          : absl::StatusOr<CommitResult>(applied.status());
                }
            }
        }
    }
    notify_role_changes();
    return result;
}

absl::Status ReplicaRuntime::demote_locked(const PrimaryToken* token) {
    if (status_.role == RuntimeRole::kStopped) {
        return absl::OkStatus();
    }
    if (token != nullptr && !token_valid_locked(*token)) {
        return absl::FailedPreconditionError("stale or invalid primary token");
    }
    auto result = release_writer_locked();
    if (status_.role != RuntimeRole::kFailed) {
        set_role_locked(RuntimeRole::kStandby);
    }
    return result;
}

absl::Status ReplicaRuntime::demote(const PrimaryToken& token) {
    absl::Status result;
    {
        std::lock_guard lock(mutex_);
        result = demote_locked(&token);
    }
    notify_role_changes();
    return result;
}

void ReplicaRuntime::stop() {
    {
        std::lock_guard lock(mutex_);
        if (status_.role == RuntimeRole::kStopped) {
            return;
        }
        (void)release_writer_locked();
        set_role_locked(RuntimeRole::kStopped);
    }
    notify_role_changes();
}

ReplicaStatus ReplicaRuntime::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

ReplicaRuntime::ReadGuard ReplicaRuntime::read(PrimaryToken token) {
    std::unique_lock lock(mutex_);
    const bool valid = token_valid_locked(token);
    return ReadGuard(std::move(lock), status_, valid, state_machine_);
}

ReplicaRuntime::PrimarySession::PrimarySession(PrimarySession&& other) noexcept
    : lifetime_(std::move(other.lifetime_)), token_(std::exchange(other.token_, {})) {}

ReplicaRuntime::PrimarySession& ReplicaRuntime::PrimarySession::operator=(
    PrimarySession&& other) noexcept {
    if (this != &other) {
        (void)demote();
        lifetime_ = std::move(other.lifetime_);
        token_ = std::exchange(other.token_, {});
    }
    return *this;
}

ReplicaRuntime::PrimarySession::~PrimarySession() {
    (void)demote();
}

absl::StatusOr<CommitResult> ReplicaRuntime::PrimarySession::submit(
    std::string request_id, std::span<const std::byte> payload) {
    auto lifetime = lifetime_.lock();
    if (lifetime == nullptr || !token_.valid()) {
        return absl::FailedPreconditionError("primary session is closed");
    }
    std::lock_guard lock(lifetime->mutex);
    return lifetime->runtime == nullptr
               ? absl::StatusOr<CommitResult>(
                     absl::FailedPreconditionError("primary session runtime is destroyed"))
               : lifetime->runtime->submit(token_, std::move(request_id), payload);
}

absl::StatusOr<CommitResult> ReplicaRuntime::PrimarySession::checkpoint() {
    auto lifetime = lifetime_.lock();
    if (lifetime == nullptr || !token_.valid()) {
        return absl::FailedPreconditionError("primary session is closed");
    }
    std::lock_guard lock(lifetime->mutex);
    return lifetime->runtime == nullptr
               ? absl::StatusOr<CommitResult>(
                     absl::FailedPreconditionError("primary session runtime is destroyed"))
               : lifetime->runtime->checkpoint(token_);
}

absl::Status ReplicaRuntime::PrimarySession::demote() {
    auto lifetime = lifetime_.lock();
    lifetime_.reset();
    const PrimaryToken token = std::exchange(token_, {});
    if (lifetime == nullptr || !token.valid()) {
        return absl::OkStatus();
    }
    std::lock_guard lock(lifetime->mutex);
    return lifetime->runtime == nullptr ? absl::OkStatus() : lifetime->runtime->demote(token);
}

} // namespace pl::minivessel
