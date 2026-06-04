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

#include "cpp/pl/sstv2/pattern/pattern_stream_vbyte.h"

#include <vector>

#include "cpp/pl/sstv2/encode/stream_vbyte.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::pattern {

absl::Status PatternStreamVByteEncoder::encode(std::span<const uint64_t> values,
                                               std::string& output) {
    // Convert uint64 to uint32 (caller ensures all values <= UINT32_MAX)
    std::vector<uint32_t> u32_values(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        u32_values[i] = static_cast<uint32_t>(values[i]);
    }

    // Format: count(varint) + StreamVByte encoded data
    std::byte count_buf[encode::Varints::kMaxVarint64Bytes];
    size_t count_bytes = encode::Varints::encode_uint64(values.size(), count_buf);

    // Encode with StreamVByte
    size_t max_size = encode::StreamVByte::max_encoded_size(u32_values.size());
    std::vector<std::byte> encoded(max_size);
    size_t encoded_size = encode::StreamVByte::encode(u32_values, encoded.data());

    output.resize(count_bytes + encoded_size);
    std::memcpy(output.data(), count_buf, count_bytes);
    std::memcpy(output.data() + count_bytes, encoded.data(), encoded_size);

    return absl::OkStatus();
}

PatternStreamVByteDecoder::PatternStreamVByteDecoder(std::span<const std::byte> data,
                                                     size_t count) {
    // Decode count varint
    auto [decoded_count, varint_bytes] = encode::Varints::decode_uint64(data);
    auto compressed = data.subspan(varint_bytes);

    // Decode all values
    std::vector<uint32_t> u32_values(decoded_count);
    encode::StreamVByte::decode(compressed, decoded_count, u32_values.data());

    // Convert to uint64
    values_.resize(decoded_count);
    for (size_t i = 0; i < decoded_count; ++i) {
        values_[i] = u32_values[i];
    }
}

uint64_t PatternStreamVByteDecoder::get(size_t index) const {
    return values_[index];
}

void PatternStreamVByteDecoder::get_batch(size_t start, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = values_[start + i];
    }
}

} // namespace pl::sstv2::pattern
