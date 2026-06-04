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

#include "cpp/pl/sstv2/pattern/pattern_none.h"

#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::pattern {

absl::Status PatternNoneEncoder::encode(std::span<const uint64_t> values, std::string& output) {
    output.resize(values.size() * sizeof(uint64_t));
    auto* dst = reinterpret_cast<std::byte*>(output.data());
    for (size_t i = 0; i < values.size(); ++i) {
        encode::encode_fixed<uint64_t>(values[i], dst + i * sizeof(uint64_t));
    }
    return absl::OkStatus();
}

PatternNoneDecoder::PatternNoneDecoder(std::span<const std::byte> data, size_t count)
    : data_(data), count_(count) {}

uint64_t PatternNoneDecoder::get(size_t index) const {
    return encode::decode_fixed<uint64_t>(data_.data() + index * sizeof(uint64_t));
}

void PatternNoneDecoder::get_batch(size_t start, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = get(start + i);
    }
}

} // namespace pl::sstv2::pattern
