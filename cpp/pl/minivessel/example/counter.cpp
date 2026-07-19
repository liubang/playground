// Copyright (c) 2026 The Authors. All rights reserved.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/local_filesystem.h"
#include "cpp/pl/minivessel/replica_runtime.h"
#include "cpp/pl/minivessel/shared_wal.h"
#include "cpp/pl/sstv2/codec/endian.h"

namespace pl::minivessel {
namespace {

std::vector<std::byte> EncodeInt64(int64_t value) {
    std::vector<std::byte> bytes(sizeof(value));
    sstv2::codec::encode_fixed64(reinterpret_cast<uint8_t*>(bytes.data()),
                                 static_cast<uint64_t>(value));
    return bytes;
}

absl::StatusOr<int64_t> DecodeInt64(std::span<const std::byte> bytes) {
    if (bytes.size() != sizeof(int64_t)) {
        return absl::DataLossError("counter record must contain exactly one int64");
    }
    return static_cast<int64_t>(
        sstv2::codec::decode_fixed64(reinterpret_cast<const uint8_t*>(bytes.data())));
}

std::string_view RoleName(RuntimeRole role) {
    switch (role) {
        case RuntimeRole::kStandby:
            return "Standby";
        case RuntimeRole::kPromoting:
            return "Promoting";
        case RuntimeRole::kPrimary:
            return "Primary";
        case RuntimeRole::kFailed:
            return "Failed";
        case RuntimeRole::kStopped:
            return "Stopped";
    }
    return "Unknown";
}

// Business-owned lifecycle hook. A real service can start Primary-only RPC admission, compaction
// scheduling, caches, or metrics here, and tear them down immediately when leaving Primary.
class CounterRoleLifecycle final : public RoleLifecycle {
public:
    explicit CounterRoleLifecycle(std::string replica_id) : replica_id_(std::move(replica_id)) {}

    void attach(ReplicaRuntime* runtime) { runtime_ = runtime; }

    void on_role_change(RuntimeRole previous, const ReplicaStatus& current) override {
        transitions_.emplace_back(previous, current.role);
        primary_resources_active_ = current.role == RuntimeRole::kPrimary;

        // RoleLifecycle runs outside the runtime mutex, so querying runtime status is safe. The
        // queried role may already be newer when multiple transitions are delivered as one batch;
        // `current` remains the immutable snapshot for this particular transition.
        const RuntimeRole observed = runtime_ == nullptr ? current.role : runtime_->status().role;
        std::cout << "  [lifecycle " << replica_id_ << "] " << RoleName(previous) << " -> "
                  << RoleName(current.role) << ", callback-observed=" << RoleName(observed)
                  << ", primary-resources=" << (primary_resources_active_ ? "started" : "stopped")
                  << '\n';
    }

    [[nodiscard]] bool primary_resources_active() const noexcept {
        return primary_resources_active_;
    }
    [[nodiscard]] const std::vector<std::pair<RuntimeRole, RuntimeRole>>& transitions() const {
        return transitions_;
    }

private:
    std::string replica_id_;
    ReplicaRuntime* runtime_ = nullptr;
    bool primary_resources_active_ = false;
    std::vector<std::pair<RuntimeRole, RuntimeRole>> transitions_;
};

class CounterStateMachine final : public ReplicatedStateMachine {
public:
    absl::StatusOr<ApplyResult> apply(const LogRecord& record,
                                      const ApplyContext& context) override {
        auto delta = DecodeInt64(record.payload);
        if (!delta.ok()) {
            return delta.status();
        }
        // Replicated state transition: both Primary and Standby must execute this identically.
        if ((*delta > 0 && value_ > std::numeric_limits<int64_t>::max() - *delta) ||
            (*delta < 0 && value_ < std::numeric_limits<int64_t>::min() - *delta)) {
            return ApplyResult::Rejected(absl::OutOfRangeError("counter overflow"));
        }
        const int64_t next_value = value_ + *delta;

        // Local-only action: the Primary may prepare an environment after durability but before
        // ACK. It must be idempotent/retryable and must not change the replicated counter result.
        if (context.is_primary_commit()) {
            ++primary_commit_preparations_;
        }
        value_ = next_value;
        return ApplyResult::Applied();
    }

    absl::StatusOr<std::vector<std::byte>> create_checkpoint() override {
        return EncodeInt64(value_);
    }

    absl::Status restore_checkpoint(std::span<const std::byte> payload,
                                    Lrsn /*checkpoint_lrsn*/) override {
        auto value = DecodeInt64(payload);
        if (!value.ok()) {
            return value.status();
        }
        value_ = *value;
        return absl::OkStatus();
    }

    [[nodiscard]] int64_t value() const noexcept { return value_; }
    [[nodiscard]] uint64_t primary_commit_preparations() const noexcept {
        return primary_commit_preparations_;
    }

private:
    int64_t value_ = 0;
    uint64_t primary_commit_preparations_ = 0;
};

absl::Status SubmitDelta(ReplicaRuntime::PrimarySession* primary,
                         std::string request_id,
                         int64_t delta) {
    const auto payload = EncodeInt64(delta);
    auto committed = primary->submit(std::move(request_id), payload);
    if (!committed.ok()) {
        return committed.status();
    }
    if (committed->apply_result.rejected()) {
        return committed->apply_result.rejection();
    }
    std::cout << "  committed delta=" << delta << " at LRSN=" << committed->lrsn.value() << '\n';
    return absl::OkStatus();
}

absl::Status SubmitRejectedDelta(ReplicaRuntime::PrimarySession* primary,
                                 std::string request_id,
                                 int64_t delta) {
    auto committed = primary->submit(std::move(request_id), EncodeInt64(delta));
    if (!committed.ok()) {
        return committed.status();
    }
    if (!committed->apply_result.rejected()) {
        return absl::InternalError("overflowing Counter mutation was unexpectedly applied");
    }
    std::cout << "  deterministically rejected delta=" << delta
              << " at LRSN=" << committed->lrsn.value() << ": "
              << committed->apply_result.rejection() << '\n';
    return absl::OkStatus();
}

absl::Status ExpectValue(std::string_view replica,
                         const CounterStateMachine& counter,
                         int64_t expected) {
    if (counter.value() != expected) {
        return absl::InternalError(std::string(replica) +
                                   " value=" + std::to_string(counter.value()) +
                                   ", expected=" + std::to_string(expected));
    }
    return absl::OkStatus();
}

absl::Status PollAndExpect(ReplicaRuntime* runtime,
                           std::string_view replica,
                           const CounterStateMachine& counter,
                           int64_t expected) {
    auto status = runtime->poll();
    return status.ok() ? ExpectValue(replica, counter, expected) : status;
}

absl::Status ExpectTransitions(std::string_view replica,
                               const CounterRoleLifecycle& lifecycle,
                               const std::vector<std::pair<RuntimeRole, RuntimeRole>>& expected) {
    if (lifecycle.transitions() != expected) {
        return absl::InternalError(std::string(replica) + " observed an unexpected role sequence");
    }
    if (lifecycle.primary_resources_active()) {
        return absl::InternalError(std::string(replica) + " leaked Primary-only resources");
    }
    return absl::OkStatus();
}

absl::Status Run() {
    const auto root = std::filesystem::temp_directory_path() / "minivessel-counter-example";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error) {
        return absl::InternalError("failed to create counter example directory: " +
                                   error.message());
    }

    LocalFileSystem filesystem;
    const FramedSharedWalOptions wal_options{
        .group = {.group_id = "counter", .incarnation = GroupIncarnation(1)},
        .path = (root / "counter.wal").string(),
    };
    // Each replica owns an independent reader/writer adapter, but both point at the same shared
    // WAL.
    FramedSharedWal wal_a(&filesystem, wal_options);
    FramedSharedWal wal_b(&filesystem, wal_options);
    CounterStateMachine counter_a;
    CounterStateMachine counter_b;
    CounterRoleLifecycle lifecycle_a("counter-a");
    CounterRoleLifecycle lifecycle_b("counter-b");
    ReplicaRuntime replica_a(&wal_a,
                             &counter_a,
                             {.replica_id = "counter-a",
                              .assignment_epoch = AssignmentEpoch(1),
                              .role_lifecycle = &lifecycle_a});
    ReplicaRuntime replica_b(&wal_b,
                             &counter_b,
                             {.replica_id = "counter-b",
                              .assignment_epoch = AssignmentEpoch(2),
                              .role_lifecycle = &lifecycle_b});
    lifecycle_a.attach(&replica_a);
    lifecycle_b.attach(&replica_b);

    std::cout << "[1] promote A and commit while B intentionally lags\n";
    auto session_a = replica_a.promote_session();
    if (!session_a.ok()) {
        return session_a.status();
    }
    absl::Status status;
    for (const auto& [id, delta] :
         std::vector<std::pair<std::string, int64_t>>{{"add-10", 10}, {"sub-3", -3}}) {
        status = SubmitDelta(&*session_a, id, delta);
        if (!status.ok()) {
            return status;
        }
    }
    if (auto checked = ExpectValue("A", counter_a, 7); !checked.ok()) {
        return checked;
    }
    if (auto checked = ExpectValue("lagging B", counter_b, 0); !checked.ok()) {
        return checked;
    }
    if (!lifecycle_a.primary_resources_active()) {
        return absl::InternalError("A did not start Primary-only resources");
    }

    std::cout
        << "[2] persist a deterministic business rejection; B rejects direct writes and tails\n";
    status = SubmitRejectedDelta(
        &*session_a, "overflow-is-a-business-rejection", std::numeric_limits<int64_t>::max());
    if (!status.ok()) {
        return status;
    }
    const auto rejected =
        replica_b.submit(session_a->token(), "illegal-standby-write", EncodeInt64(100));
    if (rejected.ok() || rejected.status().code() != absl::StatusCode::kFailedPrecondition) {
        return absl::InternalError("standby B unexpectedly accepted a write");
    }
    status = PollAndExpect(&replica_b, "B", counter_b, 7);
    if (!status.ok()) {
        return status;
    }

    std::cout << "[3] publish a checkpoint, then let B lag behind one more mutation\n";
    auto checkpoint_lrsn = session_a->checkpoint();
    if (!checkpoint_lrsn.ok()) {
        return checkpoint_lrsn.status();
    }
    status = replica_b.poll();
    if (!status.ok()) {
        return status;
    }
    status = SubmitDelta(&*session_a, "add-8-before-failover", 8);
    if (!status.ok()) {
        return status;
    }
    if (auto checked = ExpectValue("lagging B", counter_b, 7); !checked.ok()) {
        return checked;
    }

    std::cout << "[4] demote A; B catches up during promotion, writes a barrier, and serves\n";
    const PrimaryToken retired_a_token = session_a->token();
    status = session_a->demote();
    if (!status.ok()) {
        return status;
    }
    if (lifecycle_a.primary_resources_active()) {
        return absl::InternalError("A retained Primary-only resources after demotion");
    }
    auto session_b = replica_b.promote_session();
    if (!session_b.ok()) {
        return session_b.status();
    }
    if (auto checked = ExpectValue("promoted B", counter_b, 15); !checked.ok()) {
        return checked;
    }
    if (!lifecycle_b.primary_resources_active()) {
        return absl::InternalError("B did not start Primary-only resources");
    }
    const auto stale_submit = replica_a.submit(retired_a_token, "stale-a-write", EncodeInt64(100));
    if (stale_submit.ok() ||
        stale_submit.status().code() != absl::StatusCode::kFailedPrecondition) {
        return absl::InternalError("retired A token was not fenced");
    }
    status = SubmitDelta(&*session_b, "sub-4-on-b", -4);
    if (!status.ok()) {
        return status;
    }
    status = PollAndExpect(&replica_a, "demoted A", counter_a, 11);
    if (!status.ok()) {
        return status;
    }

    std::cout
        << "[5] start fresh replica C and rebuild its state from barrier/mutation/checkpoint WAL\n";
    FramedSharedWal wal_c(&filesystem, wal_options);
    CounterStateMachine counter_c;
    CounterRoleLifecycle lifecycle_c("counter-c");
    ReplicaRuntime replica_c(&wal_c,
                             &counter_c,
                             {.replica_id = "counter-c",
                              .assignment_epoch = AssignmentEpoch(3),
                              .role_lifecycle = &lifecycle_c});
    lifecycle_c.attach(&replica_c);
    status = PollAndExpect(&replica_c, "restarted C", counter_c, 11);
    if (!status.ok()) {
        return status;
    }
    if (counter_c.primary_commit_preparations() != 0) {
        return absl::InternalError("WAL replay incorrectly ran Primary-only preparation on C");
    }

    std::cout << "[6] fail over again from B to recovered C and converge all replicas\n";
    status = session_b->demote();
    if (!status.ok()) {
        return status;
    }
    auto promoted_c = replica_c.promote_session();
    if (!promoted_c.ok()) {
        return promoted_c.status();
    }
    ReplicaRuntime::PrimarySession session_c = std::move(*promoted_c);
    status = SubmitDelta(&session_c, "add-9-on-c", 9);
    if (!status.ok()) {
        return status;
    }
    status = PollAndExpect(&replica_a, "A", counter_a, 20);
    if (!status.ok()) {
        return status;
    }
    status = PollAndExpect(&replica_b, "B", counter_b, 20);
    if (!status.ok()) {
        return status;
    }
    {
        auto guard = replica_c.read(session_c.token());
        if (!guard.token_valid()) {
            return absl::InternalError("C primary token became stale");
        }
        const auto& guarded_counter =
            static_cast<const CounterStateMachine&>(guard.state_machine());
        if (auto checked = ExpectValue("C", guarded_counter, 20); !checked.ok()) {
            return checked;
        }
    }

    std::cout << "[done] values A=" << counter_a.value() << ", B=" << counter_b.value()
              << ", C=" << counter_c.value()
              << "; Primary-only preparations A=" << counter_a.primary_commit_preparations()
              << ", B=" << counter_b.primary_commit_preparations()
              << ", C=" << counter_c.primary_commit_preparations() << '\n';
    if (counter_a.primary_commit_preparations() != 3 ||
        counter_b.primary_commit_preparations() != 1 ||
        counter_c.primary_commit_preparations() != 1) {
        return absl::InternalError("Primary-only preparation counts do not match committed writes");
    }
    std::cout << "[7] drop C's RAII PrimarySession, then stop every Standby\n";
    session_c = {}; // Move-assignment releases C's still-live Primary session and demotes it.
    replica_c.stop();
    replica_b.stop();
    replica_a.stop();

    const std::vector<std::pair<RuntimeRole, RuntimeRole>> explicitly_demoted{
        {RuntimeRole::kStandby, RuntimeRole::kPromoting},
        {RuntimeRole::kPromoting, RuntimeRole::kPrimary},
        {RuntimeRole::kPrimary, RuntimeRole::kStandby},
        {RuntimeRole::kStandby, RuntimeRole::kStopped},
    };
    for (auto checked : {ExpectTransitions("A", lifecycle_a, explicitly_demoted),
                         ExpectTransitions("B", lifecycle_b, explicitly_demoted),
                         ExpectTransitions("C", lifecycle_c, explicitly_demoted)}) {
        if (!checked.ok()) {
            return checked;
        }
    }

    std::filesystem::remove_all(root, error);
    return absl::OkStatus();
}

} // namespace
} // namespace pl::minivessel

int main() {
    const auto status = pl::minivessel::Run();
    if (!status.ok()) {
        std::cerr << status << '\n';
        return 1;
    }
    return 0;
}
