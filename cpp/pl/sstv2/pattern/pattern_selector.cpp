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

#include "cpp/pl/sstv2/pattern/pattern_selector.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "cpp/pl/sstv2/pattern/pattern_constant.h"
#include "cpp/pl/sstv2/pattern/pattern_delta.h"
#include "cpp/pl/sstv2/pattern/pattern_dict.h"
#include "cpp/pl/sstv2/pattern/pattern_none.h"
#include "cpp/pl/sstv2/pattern/pattern_pfor.h"
#include "cpp/pl/sstv2/pattern/pattern_stream_vbyte.h"

namespace pl::sstv2::pattern {

namespace {

bool is_constant(std::span<const uint64_t> values) {
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] != values[0])
            return false;
    }
    return true;
}

// Returns {is_arithmetic, is_decrement, step}
struct ArithResult {
    bool is_arithmetic;
    bool is_decrement;
    uint64_t step;
};

ArithResult check_arithmetic(std::span<const uint64_t> values) {
    if (values.size() <= 1) {
        return {true, false, 0};
    }

    // Check increment
    if (values[1] >= values[0]) {
        uint64_t step = values[1] - values[0];
        for (size_t i = 2; i < values.size(); ++i) {
            if (values[i] - values[i - 1] != step) {
                goto check_decrement;
            }
        }
        return {true, false, step};
    }

check_decrement:
    // Check decrement
    if (values[0] >= values[1]) {
        uint64_t step = values[0] - values[1];
        for (size_t i = 2; i < values.size(); ++i) {
            if (values[i - 1] - values[i] != step) {
                return {false, false, 0};
            }
        }
        return {true, true, step};
    }

    return {false, false, 0};
}

bool all_fit_uint32(std::span<const uint64_t> values) {
    for (auto v : values) {
        if (v > std::numeric_limits<uint32_t>::max())
            return false;
    }
    return true;
}

size_t estimate_stream_vbyte_size(std::span<const uint64_t> values) {
    // Rough estimate: control bytes + avg 2 bytes per value
    // More accurate: count byte sizes per value
    size_t size = 10;                // varint overhead for count
    size += (values.size() + 3) / 4; // control bytes
    for (auto v : values) {
        if (v <= 0xFF)
            size += 1;
        else if (v <= 0xFFFF)
            size += 2;
        else if (v <= 0xFFFFFF)
            size += 3;
        else
            size += 4;
    }
    return size;
}

size_t estimate_pfor_size(std::span<const uint64_t> values) {
    uint64_t base = *std::min_element(values.begin(), values.end());
    std::vector<uint64_t> deltas(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        deltas[i] = values[i] - base;
    }
    std::vector<uint64_t> sorted(deltas);
    std::sort(sorted.begin(), sorted.end());
    size_t p90 = (sorted.size() * 90) / 100;
    if (p90 >= sorted.size())
        p90 = sorted.size() - 1;
    uint64_t p90_val = sorted[p90];
    uint8_t bit_width = (p90_val == 0) ? 0 : static_cast<uint8_t>(64 - __builtin_clzll(p90_val));
    uint64_t max_in_frame = (bit_width == 64) ? UINT64_MAX : ((1ULL << bit_width) - 1);

    size_t exception_count = 0;
    for (auto d : deltas) {
        if (d > max_in_frame)
            ++exception_count;
    }

    size_t packed_bytes = (values.size() * bit_width + 7) / 8;
    // header + packed + exceptions
    return sizeof(uint64_t) + 1 + 30 + packed_bytes + exception_count * (10 + sizeof(uint64_t));
}

size_t estimate_dict_size(size_t ndv, size_t count) {
    // varint + dict entries + indices
    return 10 + ndv * sizeof(uint64_t) + count;
}

} // namespace

Selection PatternSelector::select(std::span<const uint64_t> values) {
    // 1. Empty → None
    if (values.empty()) {
        return {PatternId::kNone, std::make_unique<PatternNoneEncoder>(), 0};
    }

    // 2. Constant
    if (is_constant(values)) {
        size_t size = sizeof(uint64_t) + 10; // fixed + varint
        return {PatternId::kConstant, std::make_unique<PatternConstantEncoder>(), size};
    }

    // 3. Arithmetic sequence
    auto [is_arith, is_dec, step] = check_arithmetic(values);
    if (is_arith) {
        size_t size = sizeof(uint64_t) * 2 + 10 + 1; // base + step + varint + direction
        PatternId id = is_dec ? PatternId::kDeltaDecrement : PatternId::kDeltaIncrement;
        return {id, std::make_unique<PatternDeltaEncoder>(is_dec), size};
    }

    // Gather candidates
    struct Candidate {
        PatternId id;
        std::unique_ptr<PatternEncoder> encoder;
        size_t size;
        bool random_access;
    };
    std::vector<Candidate> candidates;

    // 4. Dictionary: NDV <= 256 && count > 4
    std::unordered_set<uint64_t> unique_values(values.begin(), values.end());
    size_t ndv = unique_values.size();
    if (ndv <= 256 && values.size() > 4) {
        size_t size = estimate_dict_size(ndv, values.size());
        candidates.push_back(
            {PatternId::kDictionary, std::make_unique<PatternDictEncoder>(), size, true});
    }

    // 5. StreamVByte: all <= UINT32_MAX
    if (all_fit_uint32(values)) {
        size_t size = estimate_stream_vbyte_size(values);
        candidates.push_back(
            {PatternId::kStreamVByte, std::make_unique<PatternStreamVByteEncoder>(), size, false});
    }

    // 6. PFOR
    {
        size_t size = estimate_pfor_size(values);
        candidates.push_back(
            {PatternId::kPFor, std::make_unique<PatternPForEncoder>(), size, true});
    }

    // 7. None (raw)
    {
        size_t size = values.size() * sizeof(uint64_t);
        candidates.push_back(
            {PatternId::kNone, std::make_unique<PatternNoneEncoder>(), size, true});
    }

    // 8. Pick smallest; tie-break favors random-access
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.size != b.size)
            return a.size < b.size;
        return a.random_access > b.random_access; // prefer random access on tie
    });

    auto& best = candidates[0];
    return {best.id, std::move(best.encoder), best.size};
}

} // namespace pl::sstv2::pattern
