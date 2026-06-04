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

#include "cpp/pl/sstv2/pattern/pattern_constant.h"

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::pattern {

absl::Status PatternConstantEncoder::encode(std::span<const uint64_t> values, std::string& output) {
    if (values.empty()) {
        output.clear();
        return absl::OkStatus();
    }

    // Format: encode_fixed<uint64_t>(value) + Varints::encode_uint64(count)
    std::byte buf[sizeof(uint64_t) + encode::Varints::kMaxVarint64Bytes];
    encode::encode_fixed<uint64_t>(values[0], buf);
    size_t n = encode::Varints::encode_uint64(values.size(), buf + sizeof(uint64_t));

    output.assign(reinterpret_cast<const char*>(buf), sizeof(uint64_t) + n);
    return absl::OkStatus();
}

PatternConstantDecoder::PatternConstantDecoder(std::span<const std::byte> data, size_t count)
    : count_(count) {
    value_ = encode::decode_fixed<uint64_t>(data.data());
}

uint64_t PatternConstantDecoder::get(size_t /*index*/) const {
    return value_;
}

void PatternConstantDecoder::get_batch(size_t /*start*/, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = value_;
    }
}

} // namespace pl::sstv2::pattern
