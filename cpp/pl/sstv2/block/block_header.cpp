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

#include "cpp/pl/sstv2/block/block_header.h"

#include <cstring>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/encode/fixed.h"

namespace pl::sstv2::block {

void BlockHeader::encode(std::byte* dst) const {
    using pl::sstv2::encode::encode_fixed;
    size_t off = 0;

    encode_fixed<uint32_t>(magic, dst + off);
    off += 4;
    encode_fixed<uint32_t>(checksum, dst + off);
    off += 4;
    encode_fixed<uint32_t>(uncompressed_size, dst + off);
    off += 4;
    encode_fixed<uint32_t>(compressed_size, dst + off);
    off += 4;
    encode_fixed<uint16_t>(num_rows, dst + off);
    off += 2;
    encode_fixed<uint16_t>(num_sub_columns, dst + off);
    off += 2;
    dst[off] = static_cast<std::byte>(compression);
    off += 1;
    std::memset(dst + off, 0, 3);
    off += 3;
    encode_fixed<uint64_t>(first_row_key_offset, dst + off);
    off += 8;
    encode_fixed<uint64_t>(last_row_key_offset, dst + off);
    off += 8;
    encode_fixed<uint32_t>(data_table_size, dst + off);
    off += 4;
    encode_fixed<uint32_t>(column_store_size, dst + off);
    off += 4;
    encode_fixed<uint32_t>(offset_table_size, dst + off);
}

absl::StatusOr<BlockHeader> BlockHeader::decode(const std::byte* src) {
    using pl::sstv2::encode::decode_fixed;
    BlockHeader h;
    size_t off = 0;

    h.magic = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.checksum = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.uncompressed_size = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.compressed_size = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.num_rows = decode_fixed<uint16_t>(src + off);
    off += 2;
    h.num_sub_columns = decode_fixed<uint16_t>(src + off);
    off += 2;
    h.compression = static_cast<uint8_t>(src[off]);
    off += 1;
    std::memcpy(h.reserved, src + off, 3);
    off += 3;
    h.first_row_key_offset = decode_fixed<uint64_t>(src + off);
    off += 8;
    h.last_row_key_offset = decode_fixed<uint64_t>(src + off);
    off += 8;
    h.data_table_size = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.column_store_size = decode_fixed<uint32_t>(src + off);
    off += 4;
    h.offset_table_size = decode_fixed<uint32_t>(src + off);

    if (h.magic != kMagic) {
        return absl::InvalidArgumentError("invalid block magic");
    }
    return h;
}

} // namespace pl::sstv2::block
