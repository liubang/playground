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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cpp/pl/sstv2/pattern/pattern_constant.h"
#include "cpp/pl/sstv2/pattern/pattern_decoder.h"
#include "cpp/pl/sstv2/pattern/pattern_delta.h"
#include "cpp/pl/sstv2/pattern/pattern_dict.h"
#include "cpp/pl/sstv2/pattern/pattern_encoder.h"
#include "cpp/pl/sstv2/pattern/pattern_none.h"
#include "cpp/pl/sstv2/pattern/pattern_pfor.h"
#include "cpp/pl/sstv2/pattern/pattern_selector.h"
#include "cpp/pl/sstv2/pattern/pattern_stream_vbyte.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::pattern;

namespace {

std::span<const std::byte> AsBytes(const std::string& output) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(output.data()),
                                      output.size());
}

// Encode the values with the given encoder, build a decoder of the given
// pattern id, and verify every value round-trips through get().
void VerifyRoundTrip(PatternEncoder& encoder,
                     PatternId pattern_id,
                     const std::vector<uint64_t>& values) {
    std::string output;
    ASSERT_TRUE(encoder.encode(values, output).ok());

    auto decoder = PatternDecoder::create(pattern_id, AsBytes(output), values.size());
    ASSERT_NE(decoder, nullptr);

    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(decoder->get(i), values[i]) << "mismatch at index " << i;
    }
}

} // namespace

TEST(PatternNoneTest, RoundTrip) {
    PatternNoneEncoder encoder;
    std::vector<uint64_t> values = {0, 100, 200, 12345, UINT64_MAX};
    VerifyRoundTrip(encoder, PatternId::kNone, values);
}

TEST(PatternConstantTest, RoundTrip) {
    PatternConstantEncoder encoder;
    std::vector<uint64_t> values = {42, 42, 42, 42, 42};
    VerifyRoundTrip(encoder, PatternId::kConstant, values);
}

TEST(PatternDeltaTest, Increment) {
    PatternDeltaEncoder encoder(false);
    std::vector<uint64_t> values = {10, 20, 30, 40, 50};
    VerifyRoundTrip(encoder, PatternId::kDeltaIncrement, values);
}

TEST(PatternDeltaTest, Decrement) {
    PatternDeltaEncoder encoder(true);
    std::vector<uint64_t> values = {50, 40, 30, 20, 10};
    VerifyRoundTrip(encoder, PatternId::kDeltaDecrement, values);
}

TEST(PatternStreamVByteTest, RoundTrip) {
    PatternStreamVByteEncoder encoder;
    std::vector<uint64_t> values = {1, 127, 255, 1000, 65535, 100000};
    VerifyRoundTrip(encoder, PatternId::kStreamVByte, values);
}

TEST(PatternDictTest, RoundTrip) {
    PatternDictEncoder encoder;
    std::vector<uint64_t> values = {5, 3, 5, 5, 3, 7, 7, 3};
    VerifyRoundTrip(encoder, PatternId::kDictionary, values);
}

TEST(PatternPForTest, ClusteredSmallRange) {
    PatternPForEncoder encoder;
    std::vector<uint64_t> values = {100, 102, 101, 103, 100, 104, 99, 105};
    VerifyRoundTrip(encoder, PatternId::kPFor, values);
}

TEST(PatternPForTest, WithOutlierException) {
    PatternPForEncoder encoder;
    std::vector<uint64_t> values = {1, 2, 3, 4, 5, 1000000};
    VerifyRoundTrip(encoder, PatternId::kPFor, values);
}

TEST(PatternSelectorTest, PicksConstant) {
    std::vector<uint64_t> values = {7, 7, 7, 7, 7, 7, 7, 7};
    auto selection = PatternSelector::select(values);
    EXPECT_EQ(selection.pattern_id, PatternId::kConstant);
}

TEST(PatternSelectorTest, PicksDeltaIncrement) {
    std::vector<uint64_t> values = {10, 20, 30, 40, 50, 60, 70, 80};
    auto selection = PatternSelector::select(values);
    EXPECT_EQ(selection.pattern_id, PatternId::kDeltaIncrement);
}

TEST(PatternSelectorTest, RandomishRoundTrips) {
    std::vector<uint64_t> values = {17, 4, 9001, 42, 256, 3, 1234567, 88, 0, 65535};
    auto selection = PatternSelector::select(values);
    ASSERT_NE(selection.encoder, nullptr);
    EXPECT_EQ(selection.encoder->pattern_id(), selection.pattern_id);

    VerifyRoundTrip(*selection.encoder, selection.pattern_id, values);
}
