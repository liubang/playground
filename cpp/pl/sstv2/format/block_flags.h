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
// Created: 2026/06/13 21:00

#pragma once

#include <cstdint>

#include "cpp/pl/sstv2/compress/compress.h"

namespace pl::sstv2::format {

// =============================================================================
// Block flags: bitfield stored in block::Header::flags.
//
// Bit layout:
//   Bit  0    [P]  Pattern Store: always set in v2 blocks.
//   Bit  1    [R]  Row Key Bitmap: reserved for future use.
//   Bits 2-9  [C]  Compression codec (Codec enum, 0-255).
//   Bits 10-63     Reserved (must be 0).
// =============================================================================

namespace block_flags {

static constexpr uint64_t kPatternStore = 1ULL << 0;
static constexpr uint64_t kRowKeyBitmap = 1ULL << 1;
static constexpr uint8_t kCompressShift = 2;
static constexpr uint64_t kCompressMask = 0xFFULL << kCompressShift;

} // namespace block_flags

[[nodiscard]] constexpr uint64_t encode_block_flag(compress::Codec codec) {
    return block_flags::kPatternStore |
           (static_cast<uint64_t>(codec) << block_flags::kCompressShift);
}

[[nodiscard]] constexpr compress::Codec decode_block_flag(uint64_t flags) {
    return static_cast<compress::Codec>((flags & block_flags::kCompressMask) >>
                                        block_flags::kCompressShift);
}

} // namespace pl::sstv2::format
