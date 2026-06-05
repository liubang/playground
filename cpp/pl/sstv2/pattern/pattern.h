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
// Created: 2026/06/05 21:13

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pl::sstv2::pattern {

// =============================================================================
// PatternId: identifies the encoding pattern for a Column-Store Unit.
//
// Phase 1 implements Pattern 0 (raw) and Pattern 100 (compound).
// Patterns 1-5 are reserved for future phases.
// =============================================================================

enum class PatternId : uint8_t {
    kRaw = 0,         // Fixed-size cells, no encoding.
    kStreamVByte = 1, // Stream VByte (Phase 2).
    kPfor = 2,        // Patched Frame of Reference (Phase 3).
    kDictionary = 3,  // Dictionary encoding (Phase 3).
    kConstantInc = 4, // Constant stride increment (Phase 2).
    kConstantDec = 5, // Constant stride decrement (Phase 2).
    kCompound = 100,  // Compound: splits variable-length/composite columns into sub-columns.
};

// =============================================================================
// EncodeResult: output of a pattern encoder.
// =============================================================================

struct EncodeResult {
    std::string data; // Encoded bytes (includes pattern_id + row_count header).
    size_t row_count; // Number of rows encoded.
};

} // namespace pl::sstv2::pattern
