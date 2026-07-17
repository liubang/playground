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

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/17

#pragma once

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace pl::sstv2::io {

class FileHandle {
public:
    constexpr FileHandle() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

    friend constexpr auto operator<=>(FileHandle, FileHandle) noexcept = default;

private:
    explicit constexpr FileHandle(uint64_t value) noexcept : value_(value) {}

    uint64_t value_ = 0;

    friend class FileSystem;
};

inline constexpr FileHandle kInvalidFileHandle;

struct CreateOptions {
    bool overwrite = false;
};

// Immutable identity returned when a writable file is successfully published.
// Backends that provide content-addressed reads must populate generation and checksum.
struct FileIdentity {
    uint64_t file_id = 0;
    uint64_t content_generation = 0;
    uint64_t length = 0;
    uint64_t checksum = 0;
    bool checksum_valid = false;

    bool operator==(const FileIdentity&) const = default;
};

// Complete filesystem boundary used by SSTv2. Handles are backend-owned.
// close() always consumes a valid handle, including when it reports a late
// close or publication error; callers must not retry or reuse that handle.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    [[nodiscard]] virtual absl::StatusOr<FileHandle> create(std::string_view path,
                                                            const CreateOptions& options = {}) = 0;
    [[nodiscard]] virtual absl::StatusOr<FileHandle> open(std::string_view path) = 0;
    [[nodiscard]] virtual absl::Status append(FileHandle handle,
                                              std::span<const std::byte> data) = 0;
    // Fills destination exactly from offset without changing shared file position.
    // Returns an error for an out-of-range request or a short read.
    [[nodiscard]] virtual absl::Status read_at(FileHandle handle,
                                               uint64_t offset,
                                               std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual absl::StatusOr<uint64_t> size(FileHandle handle) = 0;
    [[nodiscard]] virtual absl::StatusOr<FileIdentity> close(FileHandle handle) = 0;
    [[nodiscard]] virtual absl::Status remove(std::string_view path) = 0;
    [[nodiscard]] virtual absl::Status rename(std::string_view source,
                                              std::string_view destination) = 0;

protected:
    [[nodiscard]] static FileHandle allocate_handle() noexcept {
        static std::atomic<uint64_t> next{1};
        return FileHandle(next.fetch_add(1, std::memory_order_relaxed));
    }
};
} // namespace pl::sstv2::io

namespace std {
template <> struct hash<pl::sstv2::io::FileHandle> {
    [[nodiscard]] size_t operator()(pl::sstv2::io::FileHandle handle) const noexcept {
        return hash<uint64_t>{}(handle.value());
    }
};
} // namespace std
