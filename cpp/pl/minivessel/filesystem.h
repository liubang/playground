// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <compare>
#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "cpp/pl/minivessel/active_log_storage.h"
#include "cpp/pl/sstv2/io/filesystem.h"

namespace pl::minivessel {

enum class ObjectStorageFeature : uint32_t {
    kNone = 0,
    kImmutableObjects = 1U << 0U,
};

class ObjectStorageCapabilities final {
public:
    constexpr ObjectStorageCapabilities() noexcept = default;
    explicit constexpr ObjectStorageCapabilities(uint32_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool has(ObjectStorageFeature feature) const noexcept {
        return (bits_ & static_cast<uint32_t>(feature)) != 0;
    }
    [[nodiscard]] constexpr bool contains(ObjectStorageCapabilities required) const noexcept {
        return (bits_ & required.bits_) == required.bits_;
    }
    [[nodiscard]] constexpr uint32_t bits() const noexcept { return bits_; }
    friend constexpr auto operator<=>(ObjectStorageCapabilities,
                                      ObjectStorageCapabilities) noexcept = default;

private:
    uint32_t bits_ = 0;
};

inline constexpr ObjectStorageCapabilities kImmutableObjectCapability(
    static_cast<uint32_t>(ObjectStorageFeature::kImmutableObjects));

inline constexpr ActiveLogCapabilities kFramedSharedWalActiveLogCapabilities(
    static_cast<uint32_t>(ActiveLogFeature::kDurableAppend) |
    static_cast<uint32_t>(ActiveLogFeature::kWriterFencing) |
    static_cast<uint32_t>(ActiveLogFeature::kDurableTail));

// Unified storage abstraction for one vessel shard.
// - immutable objects: checkpoints / sealed objects
// - optional active-log backend: fenced append + durable tail for shared WAL
class VesselFileSystem {
public:
    virtual ~VesselFileSystem() = default;

    [[nodiscard]] virtual ObjectStorageCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<sstv2::io::FileSystem> object_filesystem() const = 0;

    // Backends that do not provide active-log RPC may keep defaults (none/null).
    [[nodiscard]] virtual ActiveLogCapabilities active_log_capabilities() const noexcept {
        return ActiveLogCapabilities();
    }
    [[nodiscard]] virtual ActiveLogStorage* active_log_storage() noexcept { return nullptr; }
    [[nodiscard]] virtual const ActiveLogStorage* active_log_storage() const noexcept {
        return nullptr;
    }
};

// Backward-compatible alias for existing code that still uses the old object-only name.
using ObjectMetadataBackend = VesselFileSystem;

[[nodiscard]] inline absl::Status validate_vessel_filesystem(
    const VesselFileSystem& filesystem,
    ActiveLogCapabilities required_active_log = ActiveLogCapabilities()) {
    if (!filesystem.capabilities().contains(kImmutableObjectCapability)) {
        return absl::FailedPreconditionError(
            "filesystem lacks immutable object capability required by vessel runtime");
    }
    if (filesystem.object_filesystem() == nullptr) {
        return absl::FailedPreconditionError("filesystem has no immutable object backend");
    }

    const ActiveLogCapabilities advertised = filesystem.active_log_capabilities();
    const ActiveLogStorage* storage = filesystem.active_log_storage();
    const bool has_active_log_capabilities = advertised.bits() != 0;
    if (has_active_log_capabilities && storage == nullptr) {
        return absl::FailedPreconditionError(
            "filesystem advertises active-log capability but has no active-log backend");
    }
    if (!has_active_log_capabilities && storage != nullptr) {
        return absl::FailedPreconditionError(
            "filesystem exposes active-log backend without advertised capabilities");
    }
    if (!advertised.contains(required_active_log)) {
        return absl::FailedPreconditionError(
            "filesystem lacks required active-log capabilities");
    }
    if (required_active_log.bits() != 0 && storage == nullptr) {
        return absl::FailedPreconditionError(
            "filesystem missing active-log backend required by configured runtime");
    }
    return absl::OkStatus();
}

} // namespace pl::minivessel
