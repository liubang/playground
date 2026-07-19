// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/vessel_slice_state_machine.h"

#include <utility>

#include "absl/status/status.h"

namespace pl::minitable {

VesselSliceStateMachine::VesselSliceStateMachine(
    std::unique_ptr<SliceApplyMachine> machine,
    std::shared_ptr<const codec::CellKeyCodec> codec)
    : machine_(std::move(machine)), codec_(std::move(codec)) {}

absl::StatusOr<minivessel::ApplyResult> VesselSliceStateMachine::apply(
    const minivessel::LogRecord& record, const minivessel::ApplyContext& context) {
    static_cast<void>(context);
    if (machine_ == nullptr || codec_ == nullptr) {
        return absl::FailedPreconditionError("Vessel Slice state machine is not initialized");
    }
    if (record.type != minivessel::LogRecordType::kMutation || !record.lrsn.valid()) {
        return absl::DataLossError("Vessel Slice received an invalid mutation record");
    }

    auto result = machine_->apply_serialized(record.payload, record.lrsn.value(), *codec_);
    if (!result.ok()) {
        // Reusing a durable request identity with different bytes is a deterministic business
        // rejection. SliceApplyMachine has already advanced the apply watermark for that record.
        if (result.status().code() == absl::StatusCode::kAlreadyExists) {
            return minivessel::ApplyResult::Rejected(result.status());
        }
        return result.status();
    }
    return minivessel::ApplyResult::Applied();
}

} // namespace pl::minitable
