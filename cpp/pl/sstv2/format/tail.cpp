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
// Created: 2026/06/06 14:11

#include "cpp/pl/sstv2/format/tail.h"

#include "absl/status/status.h"
#include "cpp/pl/sstv2/codec/fixed.h"

namespace pl::sstv2::format {
namespace {

void append_tail(const Tail& tail, std::string* dst) {
    codec::append_fixed64(dst, tail.checksum);
    codec::append_fixed64(dst, tail.locator_offset);
    codec::append_fixed64(dst, tail.locator_length);
    codec::append_fixed32(dst, tail.version);
    codec::append_fixed32(dst, tail.magic);
}

} // namespace

std::string encode_tail(Tail tail) {
    tail.checksum = 0;
    std::string encoded;
    encoded.reserve(Tail::kSize);
    append_tail(tail, &encoded);
    tail.checksum = codec::crc32c_u64(encoded);
    encoded.clear();
    append_tail(tail, &encoded);
    return encoded;
}

absl::StatusOr<Tail> decode_tail(std::string_view input) {
    if (input.size() != Tail::kSize) {
        return absl::InvalidArgumentError("tail must be exactly 32 bytes");
    }

    Tail tail;
    tail.checksum = codec::read_fixed64(input, 0);
    tail.locator_offset = codec::read_fixed64(input, 8);
    tail.locator_length = codec::read_fixed64(input, 16);
    tail.version = codec::read_fixed32(input, 24);
    tail.magic = codec::read_fixed32(input, 28);

    if (tail.magic != Tail::kMagic) {
        return absl::InvalidArgumentError("tail magic mismatch");
    }
    if (tail.version != Tail::kVersion) {
        return absl::InvalidArgumentError("tail version mismatch");
    }

    Tail zero = tail;
    zero.checksum = 0;
    std::string checksum_input;
    checksum_input.reserve(Tail::kSize);
    append_tail(zero, &checksum_input);
    if (codec::crc32c_u64(checksum_input) != tail.checksum) {
        return absl::InvalidArgumentError("tail checksum mismatch");
    }
    return tail;
}

} // namespace pl::sstv2::format
