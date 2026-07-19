// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

namespace pl::minivessel {

// Strong monotonic scalar. Different protocol domains are intentionally not inter-convertible.
template <typename Tag, typename Rep = uint64_t> class MonotonicId final {
    static_assert(std::is_unsigned_v<Rep> && std::is_integral_v<Rep>);

public:
    using tag_type = Tag;
    using rep_type = Rep;

    constexpr MonotonicId() noexcept = default;
    explicit constexpr MonotonicId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

    friend constexpr auto operator<=>(MonotonicId, MonotonicId) noexcept = default;

private:
    Rep value_ = 0;
};

struct AssignmentEpochTag;
struct WriterEpochTag;
struct LrsnTag;
struct GroupIncarnationTag;
struct LeaseIdTag;
struct ByteOffsetTag;
struct PacketSequenceTag;
struct UnixTimeMillisTag;

using AssignmentEpoch = MonotonicId<AssignmentEpochTag>;
using WriterEpoch = MonotonicId<WriterEpochTag>;
using Lrsn = MonotonicId<LrsnTag>;
using GroupIncarnation = MonotonicId<GroupIncarnationTag>;
using LeaseId = MonotonicId<LeaseIdTag>;
using ByteOffset = MonotonicId<ByteOffsetTag>;
using PacketSequence = MonotonicId<PacketSequenceTag>;
using UnixTimeMillis = MonotonicId<UnixTimeMillisTag>;

struct GroupIdentity final {
    std::string group_id;
    GroupIncarnation incarnation;

    auto operator<=>(const GroupIdentity&) const = default;
};

struct Assignment final {
    GroupIdentity group;
    std::string primary_replica_id;
    AssignmentEpoch epoch;
};

struct WriterLease final {
    GroupIdentity group;
    std::string owner_instance_id;
    AssignmentEpoch assignment_epoch;
    WriterEpoch writer_epoch;
    LeaseId lease_id;
    UnixTimeMillis expires_at;
};

struct DurableBoundary final {
    GroupIdentity group;
    WriterEpoch writer_epoch;
    Lrsn durable_lrsn;
    ByteOffset durable_offset;
};

struct AppliedState final {
    GroupIdentity group;
    WriterEpoch writer_epoch;
    Lrsn applied_lrsn;
    std::string record_hash;
};

struct CheckpointIdentity final {
    GroupIdentity group;
    std::string checkpoint_id;
    Lrsn checkpoint_lrsn;
    std::string manifest_path;
    std::string manifest_checksum;
};

} // namespace pl::minivessel

namespace std {
template <typename Tag, typename Rep> struct hash<pl::minivessel::MonotonicId<Tag, Rep>> {
    [[nodiscard]] size_t operator()(pl::minivessel::MonotonicId<Tag, Rep> id) const noexcept {
        return hash<Rep>{}(id.value());
    }
};
} // namespace std
