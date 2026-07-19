#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/replica_runtime.h"

namespace pl::minivessel::e2e {

std::vector<std::byte> EncodeCounterValue(int64_t value);

class CounterStateMachine final : public ReplicatedStateMachine {
public:
    absl::StatusOr<ApplyResult> apply(const LogRecord& record,
                                      const ApplyContext& context) override;
    absl::StatusOr<std::vector<std::byte>> create_checkpoint() override;
    absl::Status restore_checkpoint(std::span<const std::byte> payload, Lrsn lrsn) override;
    [[nodiscard]] int64_t value() const noexcept { return value_; }

private:
    int64_t value_ = 0;
};

} // namespace pl::minivessel::e2e
