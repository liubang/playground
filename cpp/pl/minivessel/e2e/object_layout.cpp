#include "cpp/pl/minivessel/e2e/object_layout.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "absl/status/status.h"

namespace pl::minivessel::e2e {
namespace {

uint64_t StableHash(std::string_view value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

absl::StatusOr<std::string> EscapePathSegment(std::string_view input) {
    if (input.empty()) {
        return absl::InvalidArgumentError("path segment must not be empty");
    }
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '_' || byte == '.') {
            out << static_cast<char>(byte);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        }
    }
    const std::string result = out.str();
    if (result == "." || result == "..") {
        return absl::InvalidArgumentError("dot path segments are forbidden");
    }
    return result;
}

absl::StatusOr<std::string> GroupRoot(const GroupIdentity& group) {
    if (!group.incarnation.valid()) {
        return absl::InvalidArgumentError("group incarnation must be non-zero");
    }
    auto escaped = EscapePathSegment(group.group_id);
    if (!escaped.ok()) {
        return escaped.status();
    }
    return "/minivessel/groups/" + *escaped + "/" + std::to_string(group.incarnation.value());
}

absl::StatusOr<std::string> RecordObjectPath(const GroupIdentity& group,
                                             Lrsn lrsn,
                                             WriterEpoch epoch,
                                             LogRecordType type,
                                             std::string_view request_id) {
    if (!lrsn.valid() || !epoch.valid()) {
        return absl::InvalidArgumentError("LRSN and writer epoch must be non-zero");
    }
    auto root = GroupRoot(group);
    if (!root.ok()) {
        return root.status();
    }
    std::ostringstream object_id;
    object_id << 'e' << epoch.value() << '-' << std::hex << std::setw(16) << std::setfill('0')
              << StableHash(request_id);
    std::ostringstream path;
    path << *root << "/wal/" << (type == LogRecordType::kCheckpoint ? "checkpoints/" : "records/")
         << std::dec << std::setw(20) << std::setfill('0') << lrsn.value() << '-' << object_id.str()
         << (type == LogRecordType::kCheckpoint ? ".checkpoint" : ".record");
    return path.str();
}

} // namespace pl::minivessel::e2e
