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
// Created: 2026/06/04 14:01

#include "cpp/pl/sstv2/index/index_block_writer.h"

#include "absl/crc/crc32c.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"
#include "cpp/pl/sstv2/types/constants.h"

namespace pl::sstv2::index {

void IndexBlockWriter::add_entry(std::string_view last_key,
                                 uint64_t offset,
                                 uint32_t size,
                                 uint32_t sub_column_flags) {
    using pl::sstv2::encode::Varints;
    entries_.push_back(Entry{std::string(last_key), offset, size, sub_column_flags});
    // Rough upper bound: key length varint + key + three value varints.
    estimated_size_ += Varints::kMaxVarint32Bytes + last_key.size() + Varints::kMaxVarint64Bytes +
                       2 * Varints::kMaxVarint32Bytes;
}

std::string IndexBlockWriter::finish() const {
    using pl::sstv2::encode::encode_fixed;
    using pl::sstv2::encode::Varints;

    std::string output;
    output.reserve(8 + estimated_size_ + 4);

    std::byte buf[Varints::kMaxVarint64Bytes];

    // Header: magic (fixed 4B).
    {
        std::byte mbuf[4];
        encode_fixed<uint32_t>(types::kIndexBlockMagic, mbuf);
        output.append(reinterpret_cast<const char*>(mbuf), 4);
    }

    // Header: entry_count (varint).
    size_t n = Varints::encode_uint32(static_cast<uint32_t>(entries_.size()), buf);
    output.append(reinterpret_cast<const char*>(buf), n);

    // Entries.
    for (const auto& e : entries_) {
        n = Varints::encode_uint32(static_cast<uint32_t>(e.last_key.size()), buf);
        output.append(reinterpret_cast<const char*>(buf), n);
        output.append(e.last_key);

        n = Varints::encode_uint64(e.offset, buf);
        output.append(reinterpret_cast<const char*>(buf), n);

        n = Varints::encode_uint32(e.size, buf);
        output.append(reinterpret_cast<const char*>(buf), n);

        n = Varints::encode_uint32(e.sub_column_flags, buf);
        output.append(reinterpret_cast<const char*>(buf), n);
    }

    // Trailing checksum over everything written so far.
    auto crc = absl::ComputeCrc32c(output);
    std::byte cbuf[4];
    encode_fixed<uint32_t>(static_cast<uint32_t>(crc), cbuf);
    output.append(reinterpret_cast<const char*>(cbuf), 4);

    return output;
}

size_t IndexBlockWriter::estimated_size() const {
    // magic (4) + entry_count varint (<=5) + entries + checksum (4).
    return 4 + encode::Varints::kMaxVarint32Bytes + estimated_size_ + 4;
}

size_t IndexBlockWriter::count() const {
    return entries_.size();
}

bool IndexBlockWriter::empty() const {
    return entries_.empty();
}

std::string_view IndexBlockWriter::last_key() const {
    return entries_.back().last_key;
}

void IndexBlockWriter::reset() {
    entries_.clear();
    estimated_size_ = 0;
}

} // namespace pl::sstv2::index
