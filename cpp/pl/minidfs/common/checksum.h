// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/05/10 15:30

#pragma once

#include <cstdint>
#include <cstring>
#include <isa-l/crc.h>
#include <string_view>

namespace pl::minidfs {

enum class ChecksumType : uint8_t {
    kNone = 0,
    kCRC32C = 1,
};

/// Compute CRC32C checksum over a byte range.
/// Uses Intel ISA-L hardware-accelerated CRC32 iSCSI (same polynomial as CRC32C).
[[nodiscard]] inline uint32_t compute_crc32c(const void* data, size_t size) {
    return ::crc32_iscsi(const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data)),
                         static_cast<int>(size), 0);
}

/// Compute CRC32C checksum over a string_view.
[[nodiscard]] inline uint32_t compute_crc32c(std::string_view data) {
    return compute_crc32c(data.data(), data.size());
}

/// Extend an existing CRC32C checksum with additional data.
[[nodiscard]] inline uint32_t extend_crc32c(uint32_t crc, const void* data, size_t size) {
    return ::crc32_iscsi(const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data)),
                         static_cast<int>(size), crc);
}

/// Verify that the given checksum matches the data.
[[nodiscard]] inline bool verify_crc32c(const void* data, size_t size, uint32_t expected) {
    return compute_crc32c(data, size) == expected;
}

} // namespace pl::minidfs
