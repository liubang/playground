// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <memory>

#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/minitable/core/slice_apply_machine.h"
#include "cpp/pl/minivessel/replica_runtime.h"

namespace pl::minitable {

// MiniVessel adapter for the deterministic Slice state machine. MiniVessel LRSN is the
// authoritative apply index, so Primary commit and Standby replay execute the identical path.
class VesselSliceStateMachine final : public minivessel::ReplicatedStateMachine {
public:
    VesselSliceStateMachine(std::unique_ptr<SliceApplyMachine> machine,
                            std::shared_ptr<const codec::CellKeyCodec> codec);

    [[nodiscard]] absl::StatusOr<minivessel::ApplyResult> apply(
        const minivessel::LogRecord& record,
        const minivessel::ApplyContext& context) override;

    [[nodiscard]] SliceApplyMachine& machine() noexcept { return *machine_; }
    [[nodiscard]] const SliceApplyMachine& machine() const noexcept { return *machine_; }

private:
    std::unique_ptr<SliceApplyMachine> machine_;
    std::shared_ptr<const codec::CellKeyCodec> codec_;
};

} // namespace pl::minitable
