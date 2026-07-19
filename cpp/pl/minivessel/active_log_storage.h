// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/minivessel/types.h"

namespace pl::minivessel {

class WriterHandle final {
public:
    constexpr WriterHandle() noexcept = default;
    explicit constexpr WriterHandle(uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(WriterHandle, WriterHandle) noexcept = default;

private:
    uint64_t value_ = 0;
};

enum class ActiveLogFeature : uint32_t {
    kNone = 0,
    kDurableAppend = 1U << 0U,
    kWriterFencing = 1U << 1U,
    kDurableTail = 1U << 2U,
    kLeaseRecovery = 1U << 3U,
};

class ActiveLogCapabilities final {
public:
    constexpr ActiveLogCapabilities() noexcept = default;
    explicit constexpr ActiveLogCapabilities(uint32_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool has(ActiveLogFeature feature) const noexcept {
        return (bits_ & static_cast<uint32_t>(feature)) != 0;
    }
    [[nodiscard]] constexpr bool contains(ActiveLogCapabilities required) const noexcept {
        return (bits_ & required.bits_) == required.bits_;
    }
    [[nodiscard]] constexpr uint32_t bits() const noexcept { return bits_; }

private:
    uint32_t bits_ = 0;
};

inline constexpr ActiveLogCapabilities kAuthoritativeActiveLogCapabilities(
    static_cast<uint32_t>(ActiveLogFeature::kDurableAppend) |
    static_cast<uint32_t>(ActiveLogFeature::kWriterFencing) |
    static_cast<uint32_t>(ActiveLogFeature::kDurableTail) |
    static_cast<uint32_t>(ActiveLogFeature::kLeaseRecovery));

struct AcquireWriterRequest final {
    std::string_view path;
    std::string_view owner_instance_id;
    AssignmentEpoch assignment_epoch;
    uint64_t lease_timeout_ms = 0;
};

struct WriterSession final {
    WriterHandle handle;
    AssignmentEpoch assignment_epoch;
    WriterEpoch writer_epoch;
    LeaseId lease_id;
    UnixTimeMillis expires_at;
    ByteOffset next_offset;
    PacketSequence next_packet_sequence;
};

struct AppendOptions final {
    ByteOffset expected_offset;
    PacketSequence packet_sequence;
};

struct SyncResult final {
    WriterEpoch writer_epoch;
    ByteOffset durable_offset;
};

// Storage SPI for one append-only, fenced, durably tail-able active log. This interface carries no
// replica, role, state-machine, or checkpoint policy.
class ActiveLogStorage {
public:
    virtual ~ActiveLogStorage() = default;

    [[nodiscard]] virtual ActiveLogCapabilities active_log_capabilities() const noexcept = 0;
    [[nodiscard]] virtual absl::StatusOr<WriterSession> acquire_writer(
        const AcquireWriterRequest& request) = 0;
    [[nodiscard]] virtual absl::StatusOr<WriterSession> renew_writer(WriterHandle handle) = 0;
    [[nodiscard]] virtual absl::Status append(WriterHandle handle,
                                              const AppendOptions& options,
                                              std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual absl::StatusOr<SyncResult> sync(WriterHandle handle) = 0;
    [[nodiscard]] virtual absl::StatusOr<ByteOffset> durable_size(std::string_view path) = 0;
    [[nodiscard]] virtual absl::Status read_durable(std::string_view path,
                                                    ByteOffset offset,
                                                    std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual absl::Status seal(WriterHandle handle) = 0;
    [[nodiscard]] virtual absl::Status release_writer(WriterHandle handle) = 0;
};

[[nodiscard]] inline absl::Status validate_authoritative_active_log(
    const ActiveLogStorage& storage) {
    if (storage.active_log_capabilities().contains(kAuthoritativeActiveLogCapabilities)) {
        return absl::OkStatus();
    }
    return absl::FailedPreconditionError("storage lacks authoritative active-log capabilities");
}

} // namespace pl::minivessel

namespace std {
template <> struct hash<pl::minivessel::WriterHandle> {
    [[nodiscard]] size_t operator()(pl::minivessel::WriterHandle handle) const noexcept {
        return hash<uint64_t>{}(handle.value());
    }
};
} // namespace std
