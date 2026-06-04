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
// Created: 2026/06/04 13:06

#include "cpp/pl/sstv2/pattern/pattern_delta.h"

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::pattern {

absl::Status PatternDeltaEncoder::encode(std::span<const uint64_t> values, std::string& output) {
    if (values.empty()) {
        output.clear();
        return absl::OkStatus();
    }

    uint64_t base = values[0];
    uint64_t step = 0;
    if (values.size() > 1) {
        if (is_decrement_) {
            step = base - values[1];
        } else {
            step = values[1] - base;
        }
    }

    // Format: base(8B) + step(8B) + count(varint) + direction(1B)
    constexpr size_t kFixedPart = sizeof(uint64_t) * 2;
    std::byte buf[kFixedPart + encode::Varints::kMaxVarint64Bytes + 1];

    encode::encode_fixed<uint64_t>(base, buf);
    encode::encode_fixed<uint64_t>(step, buf + sizeof(uint64_t));

    size_t offset = kFixedPart;
    offset += encode::Varints::encode_uint64(values.size(), buf + offset);
    buf[offset] = static_cast<std::byte>(is_decrement_ ? 1 : 0);
    offset += 1;

    output.assign(reinterpret_cast<const char*>(buf), offset);
    return absl::OkStatus();
}

PatternDeltaDecoder::PatternDeltaDecoder(std::span<const std::byte> data, size_t count)
    : count_(count) {
    base_ = encode::decode_fixed<uint64_t>(data.data());
    step_ = encode::decode_fixed<uint64_t>(data.data() + sizeof(uint64_t));

    size_t offset = sizeof(uint64_t) * 2;
    auto [decoded_count, varint_bytes] = encode::Varints::decode_uint64(data.subspan(offset));
    offset += varint_bytes;

    is_decrement_ = (static_cast<uint8_t>(data[offset]) == 1);
}

uint64_t PatternDeltaDecoder::get(size_t index) const {
    if (is_decrement_) {
        return base_ - index * step_;
    }
    return base_ + index * step_;
}

void PatternDeltaDecoder::get_batch(size_t start, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = get(start + i);
    }
}

} // namespace pl::sstv2::pattern
