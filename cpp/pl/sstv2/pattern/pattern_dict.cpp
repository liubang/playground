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

#include "cpp/pl/sstv2/pattern/pattern_dict.h"

#include <unordered_map>

#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::pattern {

absl::Status PatternDictEncoder::encode(std::span<const uint64_t> values, std::string& output) {
    // Build dictionary of unique values
    std::unordered_map<uint64_t, uint8_t> value_to_index;
    std::vector<uint64_t> dict;
    std::vector<uint8_t> indices;
    indices.reserve(values.size());

    for (auto v : values) {
        auto it = value_to_index.find(v);
        if (it == value_to_index.end()) {
            uint8_t idx = static_cast<uint8_t>(dict.size());
            value_to_index[v] = idx;
            dict.push_back(v);
            indices.push_back(idx);
        } else {
            indices.push_back(it->second);
        }
    }

    // Format: dict_size(varint) + dict_entries(each 8B LE) + indices(1 byte each)
    size_t max_size =
        encode::Varints::kMaxVarint64Bytes + dict.size() * sizeof(uint64_t) + indices.size();
    output.resize(max_size);
    auto* dst = reinterpret_cast<std::byte*>(output.data());

    size_t offset = 0;
    offset += encode::Varints::encode_uint64(dict.size(), dst + offset);

    for (auto entry : dict) {
        encode::encode_fixed<uint64_t>(entry, dst + offset);
        offset += sizeof(uint64_t);
    }

    for (auto idx : indices) {
        dst[offset++] = static_cast<std::byte>(idx);
    }

    output.resize(offset);
    return absl::OkStatus();
}

PatternDictDecoder::PatternDictDecoder(std::span<const std::byte> data, size_t count) {
    size_t offset = 0;
    auto [dict_size, varint_bytes] = encode::Varints::decode_uint64(data);
    offset += varint_bytes;

    dict_.resize(dict_size);
    for (size_t i = 0; i < dict_size; ++i) {
        dict_[i] = encode::decode_fixed<uint64_t>(data.data() + offset);
        offset += sizeof(uint64_t);
    }

    indices_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        indices_[i] = static_cast<uint8_t>(data[offset + i]);
    }
}

uint64_t PatternDictDecoder::get(size_t index) const {
    return dict_[indices_[index]];
}

void PatternDictDecoder::get_batch(size_t start, size_t count, uint64_t* dst) const {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = dict_[indices_[start + i]];
    }
}

} // namespace pl::sstv2::pattern
