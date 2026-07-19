// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minitable/proto/v2/data.pb.h"

namespace pl::minitable {

struct CommitAllocation {
    Timestamp commit_ts;
    uint64_t commit_physical_ms = 0;
};

struct PreparedDataMutation {
    std::string encoded_entry;
    std::string serialized_response;
    Timestamp commit_ts;
};

[[nodiscard]] absl::StatusOr<MutationIdentity> PutIdentityV2(const proto::v2::PutRequest& request);
[[nodiscard]] absl::StatusOr<MutationIdentity> DeleteIdentityV2(
    const proto::v2::DeleteRequest& request);

// Converts one public mutation into the canonical replicated-log representation.
// Timestamp allocation, leader fencing, schema/route fencing and braft proposal are
// intentionally caller responsibilities. Phase 1 supports one GLOBAL_ORDER locality group.
[[nodiscard]] absl::StatusOr<PreparedDataMutation> PreparePutV2(
    const proto::v2::PutRequest& request,
    CommitAllocation allocation,
    uint32_t locality_group_id,
    const codec::CellKeyCodec& codec);
[[nodiscard]] absl::StatusOr<PreparedDataMutation> PrepareDeleteV2(
    const proto::v2::DeleteRequest& request,
    CommitAllocation allocation,
    uint32_t locality_group_id,
    const codec::CellKeyCodec& codec);

// Executes a point read after the caller has established the requested consistency
// barrier (for example leader/read-index for STRONG). The read timestamp is explicit,
// so this function never derives replicated semantics from a local clock.
[[nodiscard]] absl::StatusOr<std::optional<proto::v2::RowResult>> ExecuteGetV2(
    const proto::v2::GetRequest& request,
    Timestamp read_ts,
    uint32_t locality_group_id,
    const codec::CellKeyCodec& codec,
    const SliceApplyMachine& machine,
    size_t max_row_aggregate_bytes = 8 * 1024 * 1024);

} // namespace pl::minitable
