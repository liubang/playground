// Copyright (c) 2026 The Authors. All rights reserved.

#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpp/pl/minivessel/replica_runtime.h"

namespace pl::minivessel {
namespace {

class MemoryWal final : public SharedWal {
public:
    struct Storage {
        std::vector<LogRecord> records;
        uint64_t next_epoch = 1;
        MemoryWal* writer = nullptr;
    };

    explicit MemoryWal(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}

    absl::StatusOr<WriterLease> acquire_writer(std::string_view owner,
                                               AssignmentEpoch assignment_epoch,
                                               uint64_t) override {
        if (storage_->writer != nullptr) {
            return absl::AlreadyExistsError("writer held");
        }
        storage_->writer = this;
        epoch_ = WriterEpoch(storage_->next_epoch++);
        return WriterLease{.owner_instance_id = std::string(owner),
                           .assignment_epoch = assignment_epoch,
                           .writer_epoch = epoch_,
                           .lease_id = LeaseId(epoch_.value()),
                           .expires_at = UnixTimeMillis(1)};
    }
    absl::StatusOr<WriterLease> renew_writer() override {
        if (storage_->writer != this) {
            return absl::AbortedError("writer fenced");
        }
        return WriterLease{.writer_epoch = epoch_};
    }
    absl::StatusOr<DurableAppend> append(LogRecordType type,
                                         std::string request_id,
                                         std::span<const std::byte> payload) override {
        if (storage_->writer != this) {
            return absl::AbortedError("writer fenced");
        }
        if (!append_error_.ok()) {
            return append_error_;
        }
        LogRecord record{.lrsn = Lrsn(storage_->records.size() + 1),
                         .writer_epoch = epoch_,
                         .type = type,
                         .request_id = std::move(request_id),
                         .payload = std::vector<std::byte>(payload.begin(), payload.end())};
        storage_->records.push_back(record);
        return DurableAppend{.record = std::move(record),
                             .durable_offset = ByteOffset(storage_->records.size())};
    }
    absl::StatusOr<std::vector<LogRecord>> read(Lrsn first_lrsn, size_t max_records) override {
        if (!read_error_.ok()) {
            return read_error_;
        }
        std::vector<LogRecord> result;
        for (const auto& record : storage_->records) {
            if (record.lrsn >= first_lrsn && result.size() < max_records) {
                result.push_back(record);
            }
        }
        return result;
    }
    absl::StatusOr<Lrsn> durable_lrsn() override { return Lrsn(storage_->records.size()); }
    absl::Status release_writer() override {
        ++release_count;
        if (storage_->writer == this) {
            storage_->writer = nullptr;
        }
        epoch_ = {};
        return absl::OkStatus();
    }

    void set_append_error(absl::Status error) { append_error_ = std::move(error); }
    void set_read_error(absl::Status error) { read_error_ = std::move(error); }

    size_t release_count = 0;

private:
    std::shared_ptr<Storage> storage_;
    WriterEpoch epoch_;
    absl::Status append_error_;
    absl::Status read_error_;
};

class RecordingStateMachine final : public ReplicatedStateMachine {
public:
    absl::StatusOr<ApplyResult> apply(const LogRecord& record,
                                      const ApplyContext& context) override {
        records.push_back(record);
        apply_sources.push_back(context.source);
        if (!system_error.ok()) {
            return system_error;
        }
        return reject ? ApplyResult::Rejected(absl::InvalidArgumentError("business rejection"))
                      : ApplyResult::Applied();
    }
    absl::StatusOr<std::vector<std::byte>> create_checkpoint() override {
        return std::vector<std::byte>{std::byte{99}};
    }
    absl::Status restore_checkpoint(std::span<const std::byte> payload, Lrsn lrsn) override {
        restored_lrsn = lrsn;
        restored_payload.assign(payload.begin(), payload.end());
        return absl::OkStatus();
    }

    std::vector<LogRecord> records;
    std::vector<ApplySource> apply_sources;
    Lrsn restored_lrsn;
    std::vector<std::byte> restored_payload;
    absl::Status system_error;
    bool reject = false;
};

class RecordingLifecycle final : public RoleLifecycle {
public:
    void on_role_change(RuntimeRole previous, const ReplicaStatus& current) override {
        transitions.emplace_back(previous, current.role);
        if (runtime != nullptr) {
            observed_statuses.push_back(runtime->status());
        }
    }

    ReplicaRuntime* runtime = nullptr;
    std::vector<std::pair<RuntimeRole, RuntimeRole>> transitions;
    std::vector<ReplicaStatus> observed_statuses;
};

TEST(ReplicaRuntimeTest, PrimaryWalFirstAndStandbyConvergesAcrossCheckpointFailover) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal primary_wal(storage);
    MemoryWal standby_wal(storage);
    RecordingStateMachine primary_state;
    RecordingStateMachine standby_state;
    ReplicaRuntime primary(
        &primary_wal, &primary_state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    ReplicaRuntime standby(
        &standby_wal, &standby_state, {.replica_id = "b", .assignment_epoch = AssignmentEpoch(2)});

    auto primary_session = primary.promote_session();
    ASSERT_TRUE(primary_session.ok()) << primary_session.status();
    auto committed = primary_session->submit("request-1", std::vector<std::byte>{std::byte{42}});
    ASSERT_TRUE(committed.ok()) << committed.status();
    EXPECT_EQ(committed->lrsn, Lrsn(2));
    EXPECT_TRUE(committed->apply_result.applied());
    auto checkpoint = primary_session->checkpoint();
    ASSERT_TRUE(checkpoint.ok()) << checkpoint.status();
    ASSERT_TRUE(standby.poll().ok());
    EXPECT_EQ(standby_state.restored_lrsn, Lrsn(3));

    ASSERT_TRUE(primary_session->demote().ok());
    auto standby_session = standby.promote_session();
    ASSERT_TRUE(standby_session.ok()) << standby_session.status();
    EXPECT_EQ(standby.status().role, RuntimeRole::kPrimary);
    EXPECT_EQ(standby.status().applied_lrsn, Lrsn(4));
}

TEST(ReplicaRuntimeTest, BusinessRejectionAdvancesLrsnWithoutFailure) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto session = runtime.promote_session();
    ASSERT_TRUE(session.ok());
    state.reject = true;

    auto result = session->submit("rejected", {});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->apply_result.rejected());
    EXPECT_EQ(result->apply_result.rejection().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(runtime.status().applied_lrsn, result->lrsn);
    EXPECT_EQ(runtime.status().role, RuntimeRole::kPrimary);
}

TEST(ReplicaRuntimeTest, SystemApplyFailurePermanentlyFailsReplica) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto session = runtime.promote_session();
    ASSERT_TRUE(session.ok());
    state.system_error = absl::InternalError("state corruption");

    EXPECT_FALSE(session->submit("request", {}).ok());
    EXPECT_EQ(runtime.status().role, RuntimeRole::kFailed);
    EXPECT_EQ(runtime.status().applied_lrsn, Lrsn(1));
}

TEST(ReplicaRuntimeTest, StaleTokenIsRejectedAfterNewGeneration) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto first = runtime.promote_session();
    ASSERT_TRUE(first.ok());
    const PrimaryToken stale = first->token();
    ASSERT_TRUE(first->demote().ok());
    auto second = runtime.promote_session();
    ASSERT_TRUE(second.ok());

    EXPECT_FALSE(runtime.submit(stale, "stale", {}).ok());
    EXPECT_TRUE(second->submit("current", {}).ok());
}

TEST(ReplicaRuntimeTest, SessionDestructorDemotesAndLifecycleMayReenterRuntime) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    RecordingLifecycle lifecycle;
    ReplicaRuntime runtime(
        &wal,
        &state,
        {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1), .role_lifecycle = &lifecycle});
    lifecycle.runtime = &runtime;
    {
        auto session = runtime.promote_session();
        ASSERT_TRUE(session.ok());
        EXPECT_EQ(runtime.status().role, RuntimeRole::kPrimary);
    }
    EXPECT_EQ(runtime.status().role, RuntimeRole::kStandby);
    EXPECT_EQ(storage->writer, nullptr);
    ASSERT_GE(lifecycle.transitions.size(), 3);
    EXPECT_FALSE(lifecycle.observed_statuses.empty());
}

TEST(ReplicaRuntimeTest, ReadGuardExposesStateAndTokenValidity) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto session = runtime.promote_session();
    ASSERT_TRUE(session.ok());
    ASSERT_TRUE(session->submit("request", {}).ok());

    auto guard = runtime.read(session->token());
    EXPECT_TRUE(guard.token_valid());
    EXPECT_EQ(guard.status().role, RuntimeRole::kPrimary);
    EXPECT_EQ(&guard.state_machine(), &state);
    const auto& guarded_state = static_cast<const RecordingStateMachine&>(guard.state_machine());
    EXPECT_EQ(guarded_state.records.size(), 1);
}

TEST(ReplicaRuntimeTest, PromotionApplyFailureReleasesWriterExactlyOnce) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    storage->records.push_back(LogRecord{.lrsn = Lrsn(1),
                                         .writer_epoch = WriterEpoch(1),
                                         .type = LogRecordType::kMutation,
                                         .request_id = "existing"});
    MemoryWal wal(storage);
    RecordingStateMachine state;
    state.system_error = absl::InternalError("cannot replay");
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});

    EXPECT_FALSE(runtime.promote_session().ok());
    EXPECT_EQ(runtime.status().role, RuntimeRole::kFailed);
    EXPECT_EQ(wal.release_count, 1);
    runtime.stop();
    EXPECT_EQ(wal.release_count, 1);
}

TEST(ReplicaRuntimeTest, SessionMayOutliveRuntime) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    std::optional<ReplicaRuntime::PrimarySession> session;
    {
        ReplicaRuntime runtime(
            &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
        auto promoted = runtime.promote_session();
        ASSERT_TRUE(promoted.ok());
        session.emplace(std::move(*promoted));
    }

    EXPECT_FALSE(session->submit("after-destruction", {}).ok());
    session.reset();
    EXPECT_EQ(wal.release_count, 1);
}

TEST(ReplicaRuntimeTest, PromotionRejectsInvalidConfiguration) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    const auto expect_invalid = [](SharedWal* candidate_wal,
                                   ReplicatedStateMachine* candidate_state,
                                   ReplicaRuntimeOptions options) {
        ReplicaRuntime runtime(candidate_wal, candidate_state, std::move(options));
        EXPECT_EQ(runtime.promote_session().status().code(), absl::StatusCode::kInvalidArgument);
    };
    expect_invalid(nullptr, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    expect_invalid(&wal, nullptr, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    expect_invalid(&wal, &state, {.replica_id = "", .assignment_epoch = AssignmentEpoch(1)});
    expect_invalid(&wal, &state, {.replica_id = "a"});
    expect_invalid(
        &wal,
        &state,
        {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1), .writer_lease_timeout_ms = 0});
    expect_invalid(
        &wal,
        &state,
        {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1), .tail_batch_records = 0});
}

TEST(ReplicaRuntimeTest, AppendFailureClosesAdmission) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto session = runtime.promote_session();
    ASSERT_TRUE(session.ok());
    wal.set_append_error(absl::UnavailableError("sync outcome unknown"));

    EXPECT_FALSE(session->submit("request", {}).ok());
    EXPECT_EQ(runtime.status().role, RuntimeRole::kStandby);
}

TEST(ReplicaRuntimeTest, CheckpointAppendFailureReleasesWriterExactlyOnce) {
    auto storage = std::make_shared<MemoryWal::Storage>();
    MemoryWal wal(storage);
    RecordingStateMachine state;
    ReplicaRuntime runtime(
        &wal, &state, {.replica_id = "a", .assignment_epoch = AssignmentEpoch(1)});
    auto session = runtime.promote_session();
    ASSERT_TRUE(session.ok());
    wal.set_append_error(absl::UnavailableError("checkpoint sync outcome unknown"));

    EXPECT_FALSE(session->checkpoint().ok());
    EXPECT_EQ(runtime.status().role, RuntimeRole::kStandby);
    EXPECT_EQ(wal.release_count, 1);
    runtime.stop();
    EXPECT_EQ(wal.release_count, 1);
}

} // namespace
} // namespace pl::minivessel
