#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/shared_wal.h"

namespace pl::minivessel::e2e {

absl::StatusOr<std::string> EscapePathSegment(std::string_view input);
absl::StatusOr<std::string> GroupRoot(const GroupIdentity& group);
absl::StatusOr<std::string> RecordObjectPath(const GroupIdentity& group,
                                             Lrsn lrsn,
                                             WriterEpoch epoch,
                                             LogRecordType type,
                                             std::string_view request_id);

} // namespace pl::minivessel::e2e
