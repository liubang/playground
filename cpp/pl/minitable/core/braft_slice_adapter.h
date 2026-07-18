// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <memory>

#include "braft/raft.h"
#include "cpp/pl/minitable/core/slice_raft_state_machine.h"

namespace pl::minitable {

// Thin braft lifecycle adapter. The deterministic implementation remains in
// SliceRaftStateMachine so log replay and E2E tests execute the exact same path.
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

    [[nodiscard]] SliceRaftStateMachine& state_machine() noexcept { return *state_machine_; }

private:
    static constexpr const char* kSnapshotFile = "slice.snapshot";

    std::unique_ptr<SliceRaftStateMachine> state_machine_;
    std::map<uint32_t, MemTableOptions> locality_groups_;
    SliceStorePersistence persistence_;
};

} // namespace pl::minitable
