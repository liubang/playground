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
// Created: 2026/06/04 22:27

#include "cpp/pl/sstv2/file/tail.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::file {
namespace {

uint64_t TailChecksum(std::span<const std::byte, Tail::kSize> bytes) {
    std::array<std::byte, Tail::kSize> copy;
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    pl::sstv2::encode::encode_fixed<uint64_t>(0, copy.data());
    auto view = std::string_view(reinterpret_cast<const char*>(copy.data()), copy.size());
    return static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(view)));
}

} // namespace

std::array<std::byte, Tail::kSize> Tail::encode() const {
    std::array<std::byte, kSize> bytes{};
    size_t off = 0;
    encode::encode_fixed<uint64_t>(0, bytes.data() + off);
    off += 8;
    encode::encode_fixed<uint64_t>(locator_offset, bytes.data() + off);
    off += 8;
    encode::encode_fixed<uint64_t>(locator_length, bytes.data() + off);
    off += 8;
    encode::encode_fixed<uint32_t>(version, bytes.data() + off);
    off += 4;
    encode::encode_fixed<uint32_t>(magic, bytes.data() + off);
    encode::encode_fixed<uint64_t>(
        TailChecksum(std::span<const std::byte, kSize>(bytes.data(), bytes.size())), bytes.data());
    return bytes;
}

absl::StatusOr<Tail> Tail::decode(std::span<const std::byte> bytes) {
    if (bytes.size() != kSize) {
        return absl::InvalidArgumentError("SSTableV2 tail must be exactly 32 bytes");
    }

    Tail tail;
    size_t off = 0;
    tail.checksum = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    tail.locator_offset = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    tail.locator_length = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    tail.version = encode::decode_fixed<uint32_t>(bytes.data() + off);
    off += 4;
    tail.magic = encode::decode_fixed<uint32_t>(bytes.data() + off);

    if (tail.magic != kMagic) {
        return absl::InvalidArgumentError("invalid SSTableV2 tail magic");
    }
    if (tail.version != kVersion) {
        return absl::InvalidArgumentError("unsupported SSTableV2 version");
    }

    const auto expected =
        TailChecksum(std::span<const std::byte, kSize>(bytes.data(), bytes.size()));
    if (tail.checksum != expected) {
        return absl::DataLossError("SSTableV2 tail checksum mismatch");
    }
    return tail;
}

} // namespace pl::sstv2::file
