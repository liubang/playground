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

#include "cpp/pl/sstv2/pattern/pattern_pfor.h"

#include <algorithm>
#include <cstring>

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::pattern {

namespace {

uint8_t compute_bit_width(uint64_t max_val) {
    if (max_val == 0)
        return 0;
    return static_cast<uint8_t>(64 - __builtin_clzll(max_val));
}

void pack_bits(std::span<const uint64_t> deltas,
               uint8_t bit_width,
               uint64_t max_in_frame,
               std::vector<std::byte>& packed) {
    if (bit_width == 0) {
        return;
    }

    size_t total_bits = deltas.size() * bit_width;
    size_t total_bytes = (total_bits + 7) / 8;
    packed.resize(total_bytes, std::byte{0});

    size_t bit_offset = 0;
    for (size_t i = 0; i < deltas.size(); ++i) {
        uint64_t val = (deltas[i] <= max_in_frame) ? deltas[i] : 0;
        // Pack val at bit_offset with bit_width bits (little-endian bit packing)
        for (uint8_t b = 0; b < bit_width; ++b) {
            if ((val >> b) & 1) {
                size_t byte_pos = (bit_offset + b) / 8;
                size_t bit_pos = (bit_offset + b) % 8;
                packed[byte_pos] |= static_cast<std::byte>(1 << bit_pos);
            }
        }
        bit_offset += bit_width;
    }
}

} // namespace

absl::Status PatternPForEncoder::encode(std::span<const uint64_t> values, std::string& output) {
    if (values.empty()) {
        output.clear();
        return absl::OkStatus();
    }

    // Step 1: compute base and deltas
    uint64_t base = *std::min_element(values.begin(), values.end());
    std::vector<uint64_t> deltas(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        deltas[i] = values[i] - base;
    }

    // Step 2: find bit_width covering 90th percentile
    std::vector<uint64_t> sorted_deltas(deltas);
    std::sort(sorted_deltas.begin(), sorted_deltas.end());
    size_t p90_index = (sorted_deltas.size() * 90) / 100;
    if (p90_index >= sorted_deltas.size())
        p90_index = sorted_deltas.size() - 1;
    uint64_t p90_delta = sorted_deltas[p90_index];
    uint8_t bit_width = compute_bit_width(p90_delta);
    uint64_t max_in_frame = (bit_width == 64) ? UINT64_MAX : ((1ULL << bit_width) - 1);

    // Step 3: identify exceptions
    std::vector<std::pair<size_t, uint64_t>> exceptions;
    for (size_t i = 0; i < deltas.size(); ++i) {
        if (deltas[i] > max_in_frame) {
            exceptions.emplace_back(i, deltas[i]);
        }
    }

    // Step 4: pack normal values
    std::vector<std::byte> packed;
    pack_bits(deltas, bit_width, max_in_frame, packed);

    // Step 5: serialize
    // Format: base(8B) | bit_width(1B) | count(varint) | exception_count(varint) |
    //         packed_size_bytes(varint) | packed_data | exceptions
    size_t max_size = sizeof(uint64_t) + 1 + encode::Varints::kMaxVarint64Bytes * 3 +
                      packed.size() +
                      exceptions.size() * (encode::Varints::kMaxVarint64Bytes + sizeof(uint64_t));
    output.resize(max_size);
    auto* dst = reinterpret_cast<std::byte*>(output.data());
    size_t offset = 0;

    encode::encode_fixed<uint64_t>(base, dst + offset);
    offset += sizeof(uint64_t);

    dst[offset++] = static_cast<std::byte>(bit_width);

    offset += encode::Varints::encode_uint64(values.size(), dst + offset);
    offset += encode::Varints::encode_uint64(exceptions.size(), dst + offset);
    offset += encode::Varints::encode_uint64(packed.size(), dst + offset);

    std::memcpy(dst + offset, packed.data(), packed.size());
    offset += packed.size();

    // Write exceptions: (varint_index, 8B_LE_delta)
    for (auto& [idx, delta] : exceptions) {
        offset += encode::Varints::encode_uint64(idx, dst + offset);
        encode::encode_fixed<uint64_t>(delta, dst + offset);
        offset += sizeof(uint64_t);
    }

    output.resize(offset);
    return absl::OkStatus();
}

PatternPForDecoder::PatternPForDecoder(std::span<const std::byte> data, size_t /*count*/) {
    size_t offset = 0;

    base_ = encode::decode_fixed<uint64_t>(data.data() + offset);
    offset += sizeof(uint64_t);

    bit_width_ = static_cast<uint8_t>(data[offset]);
    offset += 1;

    auto [decoded_count, count_bytes] = encode::Varints::decode_uint64(data.subspan(offset));
    offset += count_bytes;

    auto [exception_count, exc_bytes] = encode::Varints::decode_uint64(data.subspan(offset));
    offset += exc_bytes;

    auto [packed_size, packed_bytes] = encode::Varints::decode_uint64(data.subspan(offset));
    offset += packed_bytes;

    packed_data_.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                        data.begin() + static_cast<std::ptrdiff_t>(offset + packed_size));
    offset += packed_size;

    // Read exceptions
    exceptions_.reserve(exception_count);
    for (size_t i = 0; i < exception_count; ++i) {
        auto [idx, idx_bytes] = encode::Varints::decode_uint64(data.subspan(offset));
        offset += idx_bytes;
        uint64_t delta = encode::decode_fixed<uint64_t>(data.data() + offset);
        offset += sizeof(uint64_t);
        exceptions_.emplace_back(idx, delta);
    }
}

uint64_t PatternPForDecoder::unpack_bits(size_t index) const {
    if (bit_width_ == 0)
        return 0;

    size_t bit_offset = index * bit_width_;
    uint64_t val = 0;
    for (uint8_t b = 0; b < bit_width_; ++b) {
        size_t byte_pos = (bit_offset + b) / 8;
        size_t bit_pos = (bit_offset + b) % 8;
        if ((static_cast<uint8_t>(packed_data_[byte_pos]) >> bit_pos) & 1) {
            val |= (1ULL << b);
        }
    }
    return val;
}

uint64_t PatternPForDecoder::get(size_t index) const {
    // Check if index is an exception (binary search since exceptions are in order)
    auto it = std::lower_bound(
        exceptions_.begin(),
        exceptions_.end(),
        index,
        [](const std::pair<size_t, uint64_t>& p, size_t idx) { return p.first < idx; });

    if (it != exceptions_.end() && it->first == index) {
        return base_ + it->second;
    }

    return base_ + unpack_bits(index);
}

void PatternPForDecoder::get_batch(size_t start, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = get(start + i);
    }
}

} // namespace pl::sstv2::pattern
