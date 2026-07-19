// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "braft/raft.h"
#include "cpp/pl/minitable/core/slice_raft_state_machine.h"

namespace pl::minitable {

// Thin braft lifecycle adapter. The deterministic implementation remains in
// SliceRaftStateMachine so log replay and E2E tests execute the exact same path.
class SliceLeadershipObserver {
public:
    virtual ~SliceLeadershipObserver() = default;
    virtual void on_slice_leader_start(int64_t term) = 0;
    virtual void on_slice_leader_stop() = 0;
};

// Result callbacks are registered by pointer identity rather than by casting arbitrary
// braft closures. This keeps follower/replay entries and non-frontend callers valid.
using ProposalApplyCallback = std::function<void(ApplyResult)>;

class BraftSliceAdapter final : public ::braft::StateMachine {
public:
    BraftSliceAdapter(std::unique_ptr<SliceRaftStateMachine> state_machine,
                      std::map<uint32_t, MemTableOptions> locality_groups,
                      SliceStorePersistence persistence)
        : state_machine_(std::move(state_machine)),
          locality_groups_(std::move(locality_groups)),
          persistence_(std::move(persistence)) {}

    void on_apply(::braft::Iterator& iterator) override;
    void on_snapshot_save(::braft::SnapshotWriter* writer, ::braft::Closure* done) override;
    int on_snapshot_load(::braft::SnapshotReader* reader) override;
    void on_leader_start(int64_t term) override;
    void on_leader_stop(const butil::Status& status) override;

    void register_proposal(::braft::Closure* closure, ProposalApplyCallback callback);
    void unregister_proposal(::braft::Closure* closure);
    void set_leadership_observer(SliceLeadershipObserver* observer);

    [[nodiscard]] SliceRaftStateMachine& state_machine() noexcept { return *state_machine_; }
    [[nodiscard]] const SliceRaftStateMachine& state_machine() const noexcept {
        return *state_machine_;
    }

private:
    static constexpr const char* kSnapshotFile = "slice.snapshot";

    std::unique_ptr<SliceRaftStateMachine> state_machine_;
    std::map<uint32_t, MemTableOptions> locality_groups_;
    SliceStorePersistence persistence_;
    std::mutex callback_mutex_;
    std::map<::braft::Closure*, ProposalApplyCallback> proposal_callbacks_;
    SliceLeadershipObserver* leadership_observer_ = nullptr;
};

} // namespace pl::minitable
