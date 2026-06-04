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

#include "cpp/pl/sstv2/pattern/pattern_decoder.h"

#include "cpp/pl/sstv2/pattern/pattern_constant.h"
#include "cpp/pl/sstv2/pattern/pattern_delta.h"
#include "cpp/pl/sstv2/pattern/pattern_dict.h"
#include "cpp/pl/sstv2/pattern/pattern_none.h"
#include "cpp/pl/sstv2/pattern/pattern_pfor.h"
#include "cpp/pl/sstv2/pattern/pattern_stream_vbyte.h"

namespace pl::sstv2::pattern {

std::unique_ptr<PatternDecoder> PatternDecoder::create(PatternId id,
                                                       std::span<const std::byte> data,
                                                       size_t count) {
    switch (id) {
        case PatternId::kNone:
            return std::make_unique<PatternNoneDecoder>(data, count);
        case PatternId::kStreamVByte:
            return std::make_unique<PatternStreamVByteDecoder>(data, count);
        case PatternId::kPFor:
            return std::make_unique<PatternPForDecoder>(data, count);
        case PatternId::kDictionary:
            return std::make_unique<PatternDictDecoder>(data, count);
        case PatternId::kDeltaIncrement:
        case PatternId::kDeltaDecrement:
            return std::make_unique<PatternDeltaDecoder>(data, count);
        case PatternId::kConstant:
            return std::make_unique<PatternConstantDecoder>(data, count);
    }
    return nullptr;
}

} // namespace pl::sstv2::pattern
