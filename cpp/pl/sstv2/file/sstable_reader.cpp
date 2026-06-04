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
// Created: 2026/06/04 22:27

#include "cpp/pl/sstv2/file/sstable_reader.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::file {
namespace {

constexpr uint32_t kDataBlockMagic = 0x4B425444; // "DTBK"
constexpr uint32_t kRootBlockMagic = 0x544F4F52; // "ROOT"
constexpr uint32_t kBloomMagic = 0x4D4F4C42;     // "BLOM"
constexpr uint64_t kFlagEmbedded = 1ULL << 63;
constexpr uint64_t kFlagTypeMask = 0x3FULL;

struct BlockHeader {
    static constexpr size_t kSize = 52;

    uint32_t magic = 0;
    uint64_t flags = 0;
    uint64_t row_count = 0;
    uint64_t offset_table_offset = 0;
    uint64_t uncompressed_size = 0;
    uint64_t compressed_size = 0;
    uint64_t checksum = 0;
};

absl::StatusOr<uint64_t> ReadVarint64(std::span<const std::byte> bytes, size_t& pos) {
    auto [value, consumed] = encode::Varints::decode_uint64(bytes.subspan(pos));
    if (consumed == 0 || pos + consumed > bytes.size()) {
        return absl::InvalidArgumentError("truncated varint");
    }
    pos += consumed;
    return value;
}

absl::StatusOr<BlockHeader> DecodeHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < BlockHeader::kSize) {
        return absl::InvalidArgumentError("block too small");
    }
    size_t off = 0;
    BlockHeader header;
    header.magic = encode::decode_fixed<uint32_t>(bytes.data() + off);
    off += 4;
    header.flags = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    header.row_count = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    header.offset_table_offset = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    header.uncompressed_size = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    header.compressed_size = encode::decode_fixed<uint64_t>(bytes.data() + off);
    off += 8;
    header.checksum = encode::decode_fixed<uint64_t>(bytes.data() + off);
    return header;
}

uint64_t BlockChecksum(std::span<const std::byte> block) {
    std::string copy(reinterpret_cast<const char*>(block.data()), block.size());
    std::fill(copy.begin() + 44, copy.begin() + 52, '\0');
    return static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(copy)));
}

uint64_t Hash64(std::string_view value, uint64_t seed) {
    uint64_t hash = 1469598103934665603ULL ^ seed;
    for (char c : value) {
        auto byte = static_cast<unsigned char>(c);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct VarlenUnit {
    std::vector<std::pair<uint64_t, uint64_t>> refs;
};

struct UintUnit {
    std::vector<uint64_t> values;
};

absl::StatusOr<VarlenUnit> DecodeVarlenUnit(std::span<const std::byte> unit) {
    if (unit.empty() || unit[0] != std::byte{0}) {
        return absl::InvalidArgumentError("unsupported varlen unit pattern");
    }
    size_t pos = 1;
    auto count_or = ReadVarint64(unit, pos);
    if (!count_or.ok()) {
        return count_or.status();
    }
    VarlenUnit decoded;
    decoded.refs.reserve(*count_or);
    for (uint64_t i = 0; i < *count_or; ++i) {
        auto offset_or = ReadVarint64(unit, pos);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        auto length_or = ReadVarint64(unit, pos);
        if (!length_or.ok()) {
            return length_or.status();
        }
        decoded.refs.push_back({*offset_or, *length_or});
    }
    return decoded;
}

absl::StatusOr<UintUnit> DecodeUintUnit(std::span<const std::byte> unit) {
    if (unit.empty() || unit[0] != std::byte{0}) {
        return absl::InvalidArgumentError("unsupported uint unit pattern");
    }
    size_t pos = 1;
    auto count_or = ReadVarint64(unit, pos);
    if (!count_or.ok()) {
        return count_or.status();
    }
    UintUnit decoded;
    decoded.values.reserve(*count_or);
    for (uint64_t i = 0; i < *count_or; ++i) {
        auto value_or = ReadVarint64(unit, pos);
        if (!value_or.ok()) {
            return value_or.status();
        }
        decoded.values.push_back(*value_or);
    }
    return decoded;
}

std::string Slice(std::span<const std::byte> block, std::pair<uint64_t, uint64_t> ref) {
    return std::string(reinterpret_cast<const char*>(block.data() + ref.first), ref.second);
}

absl::StatusOr<std::vector<std::span<const std::byte>>> DecodeUnitSpans(
    std::span<const std::byte> block, const BlockHeader& header, size_t unit_count) {
    size_t pos = header.offset_table_offset;
    std::vector<uint64_t> offsets;
    offsets.reserve(unit_count);
    for (size_t i = 0; i < unit_count; ++i) {
        auto offset_or = ReadVarint64(block, pos);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        offsets.push_back(*offset_or);
    }
    std::vector<std::span<const std::byte>> units;
    units.reserve(unit_count);
    for (size_t i = 0; i < unit_count; ++i) {
        const uint64_t begin = offsets[i];
        const uint64_t end = i + 1 == offsets.size() ? header.offset_table_offset : offsets[i + 1];
        if (begin > end || end > block.size()) {
            return absl::InvalidArgumentError("invalid block offset table");
        }
        units.push_back(block.subspan(begin, end - begin));
    }
    return units;
}

absl::StatusOr<std::vector<StoredRow>> DecodeDataBlock(std::span<const std::byte> block) {
    auto header_or = DecodeHeader(block);
    if (!header_or.ok()) {
        return header_or.status();
    }
    const BlockHeader& header = *header_or;
    if (header.magic != kDataBlockMagic) {
        return absl::InvalidArgumentError("not a data block");
    }
    if (header.uncompressed_size != block.size()) {
        return absl::InvalidArgumentError("data block length mismatch");
    }
    if (header.checksum != BlockChecksum(block)) {
        return absl::DataLossError("data block checksum mismatch");
    }

    auto units_or = DecodeUnitSpans(block, header, 9);
    if (!units_or.ok()) {
        return units_or.status();
    }
    const auto& units = *units_or;
    auto all_keys = DecodeVarlenUnit(units[0]);
    auto versions = DecodeUintUnit(units[1]);
    auto op_types = DecodeUintUnit(units[2]);
    auto flags = DecodeUintUnit(units[3]);
    auto filenames = DecodeVarlenUnit(units[4]);
    auto offsets = DecodeUintUnit(units[5]);
    auto lengths = DecodeUintUnit(units[6]);
    auto checksums = DecodeUintUnit(units[7]);
    auto embedded = DecodeVarlenUnit(units[8]);
    if (!all_keys.ok() || !versions.ok() || !op_types.ok() || !flags.ok() || !filenames.ok() ||
        !offsets.ok() || !lengths.ok() || !checksums.ok() || !embedded.ok()) {
        return absl::InvalidArgumentError("failed to decode data block units");
    }

    std::vector<StoredRow> rows;
    rows.reserve(header.row_count);
    for (size_t i = 0; i < header.row_count; ++i) {
        const uint64_t flag = flags->values[i];
        StoredRow row;
        row.all_key = Slice(block, all_keys->refs[i]);
        row.version = versions->values[i];
        row.op_type = static_cast<uint8_t>(op_types->values[i]);
        row.value_type = static_cast<types::DataType>(flag & kFlagTypeMask);
        row.embedded = (flag & kFlagEmbedded) != 0;
        row.filename = Slice(block, filenames->refs[i]);
        row.offset = offsets->values[i];
        row.length = lengths->values[i];
        row.checksum = checksums->values[i];
        row.embedded_value = Slice(block, embedded->refs[i]);
        rows.push_back(std::move(row));
    }
    return rows;
}

absl::StatusOr<std::vector<std::pair<uint64_t, uint64_t>>> DecodeRootIndex(
    std::span<const std::byte> block) {
    auto header_or = DecodeHeader(block);
    if (!header_or.ok()) {
        return header_or.status();
    }
    const BlockHeader& header = *header_or;
    if (header.magic != kRootBlockMagic) {
        return absl::InvalidArgumentError("not a root index block");
    }
    if (header.checksum != BlockChecksum(block)) {
        return absl::DataLossError("root index checksum mismatch");
    }
    auto units_or = DecodeUnitSpans(block, header, 4);
    if (!units_or.ok()) {
        return units_or.status();
    }
    auto offsets = DecodeUintUnit((*units_or)[2]);
    auto lengths = DecodeUintUnit((*units_or)[3]);
    if (!offsets.ok() || !lengths.ok()) {
        return absl::InvalidArgumentError("failed to decode root index");
    }
    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    for (size_t i = 0; i < offsets->values.size(); ++i) {
        blocks.push_back({offsets->values[i], lengths->values[i]});
    }
    return blocks;
}

absl::StatusOr<bool> BloomMayContain(std::span<const std::byte> bloom, std::string_view all_key) {
    if (bloom.size() < 36) {
        return absl::InvalidArgumentError("bloom filter too small");
    }
    const uint32_t magic = encode::decode_fixed<uint32_t>(bloom.data());
    if (magic != kBloomMagic) {
        return absl::InvalidArgumentError("invalid bloom filter magic");
    }
    const uint32_t num_hashes = encode::decode_fixed<uint32_t>(bloom.data() + 8);
    const uint64_t num_bits = encode::decode_fixed<uint64_t>(bloom.data() + 12);
    const uint64_t checksum = encode::decode_fixed<uint64_t>(bloom.data() + 28);
    if ((num_bits + 7) / 8 + 36 != bloom.size()) {
        return absl::InvalidArgumentError("bloom filter length mismatch");
    }
    std::string copy(reinterpret_cast<const char*>(bloom.data()), bloom.size());
    std::fill(copy.begin() + 28, copy.begin() + 36, '\0');
    if (static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(copy))) != checksum) {
        return absl::DataLossError("bloom filter checksum mismatch");
    }

    const auto* bits = reinterpret_cast<const unsigned char*>(bloom.data() + 36);
    const uint64_t h1 = Hash64(all_key, 0x9e3779b97f4a7c15ULL);
    const uint64_t h2 = Hash64(all_key, 0xc2b2ae3d27d4eb4fULL) | 1ULL;
    for (uint32_t i = 0; i < num_hashes; ++i) {
        const uint64_t bit = (h1 + i * h2) % num_bits;
        if ((bits[bit / 8] & static_cast<unsigned char>(1U << (bit % 8))) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

absl::StatusOr<SSTableReader> SSTableReader::open(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return absl::NotFoundError("failed to open SSTableV2 key file");
    }

    SSTableReader reader;
    reader.key_file_path_ = path;
    std::ostringstream ss;
    ss << in.rdbuf();
    reader.data_ = ss.str();
    if (reader.data_.size() < Tail::kSize) {
        return absl::InvalidArgumentError("key file too small for SSTableV2 tail");
    }

    auto all_bytes = reader.file_data();
    auto tail_or = Tail::decode(all_bytes.subspan(all_bytes.size() - Tail::kSize, Tail::kSize));
    if (!tail_or.ok()) {
        return tail_or.status();
    }
    reader.tail_ = *tail_or;
    if (reader.tail_.locator_offset + reader.tail_.locator_length >
        reader.data_.size() - Tail::kSize) {
        return absl::InvalidArgumentError("locator extends beyond key file");
    }

    auto locator_or = Locator::decode(
        all_bytes.subspan(reader.tail_.locator_offset, reader.tail_.locator_length));
    if (!locator_or.ok()) {
        return locator_or.status();
    }
    reader.locator_ = *locator_or;
    if (!reader.locator_.find_section("RootIndex").has_value()) {
        return absl::InvalidArgumentError("locator does not contain RootIndex");
    }
    reader.valid_ = true;
    return reader;
}

bool SSTableReader::is_valid() const {
    return valid_;
}

const Tail& SSTableReader::tail() const {
    return tail_;
}

const Locator& SSTableReader::locator() const {
    return locator_;
}

std::span<const std::byte> SSTableReader::file_data() const {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data_.data()),
                                      data_.size());
}

std::vector<StoredRow> SSTableReader::scan() const {
    std::vector<StoredRow> rows;
    auto root = locator_.find_section("RootIndex");
    if (!root.has_value()) {
        return rows;
    }
    auto bytes = file_data();
    auto blocks_or = DecodeRootIndex(bytes.subspan(root->offset, root->length));
    if (!blocks_or.ok()) {
        return rows;
    }
    for (const auto& [offset, length] : *blocks_or) {
        auto block_rows = DecodeDataBlock(bytes.subspan(offset, length));
        if (!block_rows.ok()) {
            return {};
        }
        rows.insert(rows.end(),
                    std::make_move_iterator(block_rows->begin()),
                    std::make_move_iterator(block_rows->end()));
    }
    return rows;
}

absl::StatusOr<std::string> SSTableReader::read_value_bytes(const StoredRow& row) const {
    if (row.embedded) {
        if (row.embedded_value.size() != row.length) {
            return absl::DataLossError("embedded value length mismatch");
        }
        if (static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(row.embedded_value))) !=
            row.checksum) {
            return absl::DataLossError("embedded value checksum mismatch");
        }
        return row.embedded_value;
    }
    std::ifstream in(row.filename, std::ios::binary);
    if (!in) {
        return absl::NotFoundError("failed to open SSTableV2 value file");
    }
    in.seekg(static_cast<std::streamoff>(row.offset));
    std::string bytes(row.length, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return absl::DataLossError("failed to read separated SSTableV2 value");
    }
    if (static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(bytes))) != row.checksum) {
        return absl::DataLossError("separated value checksum mismatch");
    }
    return bytes;
}

absl::StatusOr<bool> SSTableReader::may_contain_encoded_key(std::string_view all_key) const {
    auto bloom = locator_.find_section("BloomFilter0");
    if (!bloom.has_value()) {
        return true;
    }
    auto bytes = file_data();
    if (bloom->offset + bloom->length > bytes.size()) {
        return absl::InvalidArgumentError("bloom filter extends beyond key file");
    }
    return BloomMayContain(bytes.subspan(bloom->offset, bloom->length), all_key);
}

} // namespace pl::sstv2::file
