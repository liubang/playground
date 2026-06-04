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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cpp/pl/sstv2/pattern/pattern_decoder.h"
#include "cpp/pl/sstv2/pattern/pattern_encoder.h"

namespace pl::sstv2::pattern {

class PatternPForEncoder : public PatternEncoder {
public:
    absl::Status encode(std::span<const uint64_t> values, std::string& output) override;
    PatternId pattern_id() const override { return PatternId::kPFor; }
};

class PatternPForDecoder : public PatternDecoder {
public:
    PatternPForDecoder(std::span<const std::byte> data, size_t count);

    uint64_t get(size_t index) const override;
    void get_batch(size_t start, size_t count, uint64_t* dst) const override;
    bool supports_random_access() const override { return true; }

private:
    uint64_t unpack_bits(size_t index) const;

    uint64_t base_;
    uint8_t bit_width_;
    std::vector<std::byte> packed_data_;
    std::vector<std::pair<size_t, uint64_t>> exceptions_; // (index, delta)
};

} // namespace pl::sstv2::pattern
