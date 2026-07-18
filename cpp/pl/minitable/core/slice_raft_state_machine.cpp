// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/slice_raft_state_machine.h"

#include <utility>

namespace pl::minitable {

absl::StatusOr<ApplyResult> SliceRaftStateMachine::on_apply(uint64_t index,
                                                            uint64_t term,
                                                            std::span<const std::byte> data) {
    return apply_committed(index, term, data, false);
}

absl::StatusOr<ApplyResult> SliceRaftStateMachine::on_braft_apply(
    uint64_t index, uint64_t term, std::span<const std::byte> data) {
    return apply_committed(index, term, data, true);
}

absl::StatusOr<ApplyResult> SliceRaftStateMachine::apply_committed(
    uint64_t index, uint64_t term, std::span<const std::byte> data, bool allow_index_gap) {
    std::lock_guard lock(mutex_);
    const uint64_t visible_index = machine_->store().visible_applied_index();
    const bool index_is_valid = allow_index_gap ? index > visible_index : index == visible_index + 1;
    if (term == 0 || index == 0 || !index_is_valid) {
        return absl::DataLossError("committed Raft entry has an invalid state-machine index");
    }
    auto result = machine_->apply_serialized(data, index, *codec_);
    if (result.ok()) {
        last_applied_term_ = term;
    }
    return result;
}

absl::StatusOr<std::string> SliceRaftStateMachine::on_snapshot_save() const {
    std::lock_guard lock(mutex_);
    if (last_applied_term_ == 0) {
        return absl::FailedPreconditionError("Snapshot has no applied Raft term");
    }
    auto metadata = metadata_;
    metadata.last_included_index = machine_->store().visible_applied_index();
    metadata.last_included_term = last_applied_term_;
    return EncodeSliceSnapshot(*machine_, metadata);
}

absl::Status SliceRaftStateMachine::on_snapshot_load(
    std::span<const std::byte> data,
    std::map<uint32_t, MemTableOptions> locality_groups,
    SliceStorePersistence persistence) {
    auto loaded = InstallSliceSnapshot(data, std::move(locality_groups), std::move(persistence));
    if (!loaded.ok()) {
        return loaded.status();
    }
    std::lock_guard lock(mutex_);
    if (loaded->metadata.table_id != metadata_.table_id ||
        loaded->metadata.slice_id != metadata_.slice_id ||
        loaded->metadata.schema_version != metadata_.schema_version ||
        loaded->metadata.route_epoch != metadata_.route_epoch ||
        loaded->metadata.replica_set_epoch != metadata_.replica_set_epoch) {
        return absl::FailedPreconditionError("Snapshot route or schema fence mismatch");
    }
    machine_ = std::move(loaded->machine);
    metadata_ = loaded->metadata;
    last_applied_term_ = metadata_.last_included_term;
    return absl::OkStatus();
}

absl::Status SliceRaftStateMachine::replay(
    std::span<const std::pair<uint64_t, std::string>> committed_entries,
    uint64_t term) {
    uint64_t previous = machine_->store().visible_applied_index();
    for (const auto& [index, data] : committed_entries) {
        if (index != previous + 1) {
            return absl::DataLossError("committed Raft replay contains an index gap");
        }
        const auto bytes = std::as_bytes(std::span(data.data(), data.size()));
        auto result = on_apply(index, term, bytes);
        if (!result.ok()) {
            return result.status();
        }
        previous = index;
    }
    return absl::OkStatus();
}

} // namespace pl::minitable
