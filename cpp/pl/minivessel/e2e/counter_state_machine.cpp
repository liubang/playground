#include "cpp/pl/minivessel/e2e/counter_state_machine.h"

#include <cstring>
#include <limits>

#include "absl/status/status.h"

namespace pl::minivessel::e2e {
namespace {

absl::StatusOr<int64_t> Decode(std::span<const std::byte> payload) {
    if (payload.size() != sizeof(int64_t)) {
        return absl::DataLossError("counter payload must be an int64");
    }
    uint64_t bits = 0;
    for (size_t i = 0; i < sizeof(bits); ++i) {
        bits |= static_cast<uint64_t>(std::to_integer<uint8_t>(payload[i])) << (i * 8);
    }
    return static_cast<int64_t>(bits);
}

} // namespace

std::vector<std::byte> EncodeCounterValue(int64_t value) {
    const uint64_t bits = static_cast<uint64_t>(value);
    std::vector<std::byte> output(sizeof(bits));
    for (size_t i = 0; i < sizeof(bits); ++i)
        output[i] = std::byte((bits >> (i * 8)) & 0xff);
    return output;
}

absl::StatusOr<ApplyResult> CounterStateMachine::apply(const LogRecord& record,
                                                       const ApplyContext&) {
    auto delta = Decode(record.payload);
    if (!delta.ok())
        return delta.status();
    if ((*delta > 0 && value_ > std::numeric_limits<int64_t>::max() - *delta) ||
        (*delta < 0 && value_ < std::numeric_limits<int64_t>::min() - *delta)) {
        return ApplyResult::Rejected(absl::OutOfRangeError("counter overflow"));
    }
    value_ += *delta;
    return ApplyResult::Applied();
}

absl::StatusOr<std::vector<std::byte>> CounterStateMachine::create_checkpoint() {
    return EncodeCounterValue(value_);
}

absl::Status CounterStateMachine::restore_checkpoint(std::span<const std::byte> payload, Lrsn) {
    auto value = Decode(payload);
    if (!value.ok())
        return value.status();
    value_ = *value;
    return absl::OkStatus();
}

} // namespace pl::minivessel::e2e
