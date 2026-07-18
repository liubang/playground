// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "absl/status/statusor.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/proto/v2/internal.pb.h"

namespace pl::minitable {

inline constexpr uint32_t kSliceSnapshotFormatVersion = 1;

struct SliceSnapshotMetadata {
    uint64_t table_id = 0;
    uint64_t slice_id = 0;
    uint64_t schema_version = 0;
    uint64_t route_epoch = 0;
    uint64_t replica_set_epoch = 0;
    uint64_t last_included_index = 0;
    uint64_t last_included_term = 0;
    uint64_t dedupe_retention_floor = 0;
};

struct LoadedSliceSnapshot {
    std::unique_ptr<SliceApplyMachine> machine;
    SliceSnapshotMetadata metadata;
};

[[nodiscard]] absl::StatusOr<std::string> EncodeSliceSnapshot(
    const SliceApplyMachine& machine, const SliceSnapshotMetadata& metadata);
[[nodiscard]] absl::StatusOr<LoadedSliceSnapshot> InstallSliceSnapshot(
    std::span<const std::byte> encoded,
    std::map<uint32_t, MemTableOptions> locality_groups,
    SliceStorePersistence persistence);

} // namespace pl::minitable
