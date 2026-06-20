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
// Created: 2026/06/05 22:09

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/compress/compress.h"
#include "cpp/pl/sstv2/types/internal_row.h"

namespace pl::sstv2::block {

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

enum class Kind : uint32_t {
    kData = 0x4B425444,      // DTBK
    kIndex = 0x4B425849,     // IXBK
    kRootIndex = 0x544F4F52, // ROOT
};

struct Header {
    static constexpr size_t kSize = 52;

    Kind magic = Kind::kData;
    uint64_t flags = 0;
    uint64_t row_count = 0;
    uint64_t offset_table_offset = 0;
    uint64_t uncompressed_block_length = 0;
    uint64_t compressed_block_length = 0;
    uint64_t checksum = 0;
};

struct Options {
    Kind kind = Kind::kData;
    compress::Options compression;
    uint64_t max_block_size_soft_limit = 64 * 1024;
    uint64_t max_block_size_hard_limit = 128 * 1024;
    uint64_t max_row_count = 4096;
};

class BlockBuilder {
public:
    BlockBuilder(types::InternalSchema::ConstRef schema, Options options);

    [[nodiscard]] absl::Status add(types::InternalRow row);
    [[nodiscard]] absl::Status add(types::InternalRow row, std::string embedded_value);
    [[nodiscard]] absl::StatusOr<std::string> finish();

    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }

private:
    types::InternalSchema::ConstRef schema_;
    Options options_;
    std::vector<types::InternalRow> rows_;
    std::vector<std::string> embedded_values_;
};

class BlockReader {
public:
    [[nodiscard]] static absl::StatusOr<BlockReader> open(
        std::string_view block, const types::InternalSchema::ConstRef& schema, Kind expected);

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<types::InternalRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] absl::StatusOr<std::string_view> embedded_value(
        size_t row_index, const types::InternalSchema::ConstRef& schema) const;

private:
    Header header_;
    std::vector<types::InternalRow> rows_;
    std::string data_table_;
};

} // namespace pl::sstv2::block
