// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <compare>
#include <cstdint>
#include <memory>

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
    [[nodiscard]] constexpr uint32_t bits() const noexcept { return bits_; }
    friend constexpr auto operator<=>(ObjectStorageCapabilities,
                                      ObjectStorageCapabilities) noexcept = default;

private:
    uint32_t bits_ = 0;
};

// Neutral immutable checkpoint/sealed-object backend. Election and active-log authority are
// deliberately separate services and are not implied by object storage.
class ObjectMetadataBackend {
public:
    virtual ~ObjectMetadataBackend() = default;

    [[nodiscard]] virtual ObjectStorageCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<sstv2::io::FileSystem> object_filesystem() const = 0;
};

} // namespace pl::minivessel
