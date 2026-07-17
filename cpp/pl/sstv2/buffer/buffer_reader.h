// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace pl::sstv2::buffer {

class BufferReader {
public:
    constexpr BufferReader() noexcept = default;
    explicit constexpr BufferReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] static BufferReader from_string(std::string_view value) noexcept {
        return BufferReader(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] constexpr size_t position() const noexcept { return position_; }
    [[nodiscard]] constexpr size_t remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return remaining() == 0; }
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] constexpr std::span<const std::byte> unread_bytes() const noexcept {
        return bytes_.subspan(position_);
    }

    [[nodiscard]] absl::Status seek(size_t position) noexcept {
        if (position > bytes_.size()) {
            return absl::OutOfRangeError("buffer seek position is out of range");
        }
        position_ = position;
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status skip(size_t length) noexcept {
        if (length > remaining()) {
            return absl::OutOfRangeError("buffer skip length is out of range");
        }
        position_ += length;
        return absl::OkStatus();
    }

    [[nodiscard]] absl::StatusOr<std::span<const std::byte>> peek(size_t length) const noexcept {
        if (length > remaining()) {
            return absl::OutOfRangeError("buffer read length is out of range");
        }
        return bytes_.subspan(position_, length);
    }

    [[nodiscard]] absl::StatusOr<std::span<const std::byte>> read(size_t length) noexcept {
        auto result = peek(length);
        if (!result.ok()) {
            return result.status();
        }
        position_ += length;
        return *result;
    }

private:
    std::span<const std::byte> bytes_;
    size_t position_ = 0;
};

} // namespace pl::sstv2::buffer
