// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/slice_snapshot.h"

namespace pl::minitable {

// Storage-independent committed-log adapter. A braft on_apply callback supplies
// iterator index/term/data to this class; tests and recovery use the same path.
class SliceRaftStateMachine final {
public:
    SliceRaftStateMachine(std::unique_ptr<SliceApplyMachine> machine,
                          std::shared_ptr<const codec::CellKeyCodec> codec,
                          SliceSnapshotMetadata metadata)
        : machine_(std::move(machine)), codec_(std::move(codec)), metadata_(metadata) {}

    // Strict contiguous replay entry point used by deterministic recovery tests.
    [[nodiscard]] absl::StatusOr<ApplyResult> on_apply(uint64_t index,
                                                       uint64_t term,
                                                       std::span<const std::byte> data);
    // braft may consume configuration log indexes without delivering them to the user FSM.
    [[nodiscard]] absl::StatusOr<ApplyResult> on_braft_apply(uint64_t index,
                                                             uint64_t term,
                                                             std::span<const std::byte> data);
    [[nodiscard]] absl::StatusOr<std::string> on_snapshot_save() const;
    [[nodiscard]] absl::Status on_snapshot_load(
        std::span<const std::byte> data,
        std::map<uint32_t, MemTableOptions> locality_groups,
        SliceStorePersistence persistence);
    [[nodiscard]] absl::Status replay(
        std::span<const std::pair<uint64_t, std::string>> committed_entries,
        uint64_t term);

    [[nodiscard]] SliceApplyMachine& machine() noexcept { return *machine_; }
    [[nodiscard]] const SliceApplyMachine& machine() const noexcept { return *machine_; }
private:
    [[nodiscard]] absl::StatusOr<ApplyResult> apply_committed(
        uint64_t index, uint64_t term, std::span<const std::byte> data, bool allow_index_gap);

    mutable std::mutex mutex_;
    std::unique_ptr<SliceApplyMachine> machine_;
    std::shared_ptr<const codec::CellKeyCodec> codec_;
    SliceSnapshotMetadata metadata_;
    uint64_t last_applied_term_ = 0;
};

} // namespace pl::minitable
