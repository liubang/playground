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
// Created: 2026/06/04 15:23

#include "cpp/pl/sstv2/file/tail.h"

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/types/constants.h"

namespace pl::sstv2::file {

void Tail::encode_to(std::byte* dst) const {
    using pl::sstv2::encode::encode_fixed;
    size_t off = 0;

    encode_fixed<uint32_t>(magic, dst + off);
    off += 4;
    encode_fixed<uint16_t>(format_version, dst + off);
    off += 2;
    encode_fixed<uint16_t>(reserved, dst + off);
    off += 2;
    encode_fixed<uint64_t>(locator_offset, dst + off);
    off += 8;
    encode_fixed<uint32_t>(locator_size, dst + off);
    off += 4;
    encode_fixed<uint32_t>(locator_checksum, dst + off);
    off += 4;
    encode_fixed<uint64_t>(file_checksum, dst + off);
}

absl::StatusOr<Tail> Tail::decode_from(std::span<const std::byte> src) {
    using pl::sstv2::encode::decode_fixed;

    if (src.size() < kSize) {
        return absl::InvalidArgumentError("tail data too short");
    }

    Tail t;
    size_t off = 0;

    t.magic = decode_fixed<uint32_t>(src.data() + off);
    off += 4;
    t.format_version = decode_fixed<uint16_t>(src.data() + off);
    off += 2;
    t.reserved = decode_fixed<uint16_t>(src.data() + off);
    off += 2;
    t.locator_offset = decode_fixed<uint64_t>(src.data() + off);
    off += 8;
    t.locator_size = decode_fixed<uint32_t>(src.data() + off);
    off += 4;
    t.locator_checksum = decode_fixed<uint32_t>(src.data() + off);
    off += 4;
    t.file_checksum = decode_fixed<uint64_t>(src.data() + off);

    if (t.magic != types::kSstMagic) {
        return absl::InvalidArgumentError("invalid SST magic number");
    }

    return t;
}

} // namespace pl::sstv2::file
