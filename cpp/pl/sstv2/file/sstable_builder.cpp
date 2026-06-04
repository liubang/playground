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

#include "cpp/pl/sstv2/file/sstable_builder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "cpp/pl/sstv2/encode/fixed.h"
#include "cpp/pl/sstv2/encode/varints.h"
#include "cpp/pl/sstv2/file/locator.h"
#include "cpp/pl/sstv2/file/tail.h"

namespace pl::sstv2::file {
namespace {

constexpr uint32_t kDataBlockMagic = 0x4B425444; // "DTBK"
constexpr uint32_t kRootBlockMagic = 0x544F4F52; // "ROOT"
constexpr uint32_t kBloomMagic = 0x4D4F4C42;     // "BLOM"
constexpr uint32_t kBloomVersion = 1;
constexpr uint32_t kConfigurationMagic = 0x47494643; // "CFIG"
constexpr uint32_t kSchemaMagic = 0x414D4553;        // "SEMA"
constexpr uint32_t kStatisticsMagic = 0x54415453;    // "STAT"
constexpr uint32_t kCompatibilityMagic = 0x504D4F43; // "COMP"
constexpr uint64_t kFlagEmbedded = 1ULL << 63;

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

void AppendFixed32(uint32_t value, std::string& out) {
    std::array<std::byte, sizeof(uint32_t)> bytes{};
    encode::encode_fixed<uint32_t>(value, bytes.data());
    out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void AppendFixed64(uint64_t value, std::string& out) {
    std::array<std::byte, sizeof(uint64_t)> bytes{};
    encode::encode_fixed<uint64_t>(value, bytes.data());
    out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void AppendVarint64(uint64_t value, std::string& out) {
    std::array<std::byte, encode::Varints::kMaxVarint64Bytes> bytes{};
    const size_t n = encode::Varints::encode_uint64(value, bytes.data());
    out.append(reinterpret_cast<const char*>(bytes.data()), n);
}

void AppendHeader(const BlockHeader& header, std::string& out) {
    AppendFixed32(header.magic, out);
    AppendFixed64(header.flags, out);
    AppendFixed64(header.row_count, out);
    AppendFixed64(header.offset_table_offset, out);
    AppendFixed64(header.uncompressed_size, out);
    AppendFixed64(header.compressed_size, out);
    AppendFixed64(header.checksum, out);
}

uint64_t Hash64(std::string_view value, uint64_t seed) {
    // FNV-1a with a seed is enough for the file-format test bloom filter. The
    // serialized format is independent from the hash implementation version.
    uint64_t hash = 1469598103934665603ULL ^ seed;
    for (char c : value) {
        auto byte = static_cast<unsigned char>(c);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t Crc64(std::string_view data) {
    return static_cast<uint64_t>(static_cast<uint32_t>(absl::ComputeCrc32c(data)));
}

uint64_t BlockChecksum(std::string_view block) {
    std::string copy(block);
    std::fill(copy.begin() + 44, copy.begin() + 52, '\0');
    return Crc64(copy);
}

std::string BuildBlock(uint32_t magic,
                       uint64_t row_count,
                       std::string data_table,
                       std::vector<std::string> units) {
    std::string block(BlockHeader::kSize, '\0');
    block.append(data_table);

    std::vector<uint64_t> offsets;
    offsets.reserve(units.size());
    for (auto& unit : units) {
        offsets.push_back(block.size());
        block.append(unit);
    }

    const uint64_t offset_table_offset = block.size();
    for (uint64_t offset : offsets) {
        AppendVarint64(offset, block);
    }

    BlockHeader header{
        .magic = magic,
        .flags = 0,
        .row_count = row_count,
        .offset_table_offset = offset_table_offset,
        .uncompressed_size = block.size(),
        .compressed_size = 0,
        .checksum = 0,
    };
    std::string encoded_header;
    AppendHeader(header, encoded_header);
    std::copy(encoded_header.begin(), encoded_header.end(), block.begin());

    header.checksum = BlockChecksum(block);
    encoded_header.clear();
    AppendHeader(header, encoded_header);
    std::copy(encoded_header.begin(), encoded_header.end(), block.begin());
    return block;
}

std::string EncodeVarlenUnit(std::span<const std::pair<uint64_t, uint64_t>> values) {
    std::string out;
    out.push_back('\0'); // pattern 0: no pattern.
    AppendVarint64(values.size(), out);
    for (const auto& [offset, length] : values) {
        AppendVarint64(offset, out);
        AppendVarint64(length, out);
    }
    return out;
}

std::string EncodeUintUnit(std::span<const uint64_t> values) {
    std::string out;
    out.push_back('\0'); // pattern 0: no pattern.
    AppendVarint64(values.size(), out);
    for (uint64_t value : values) {
        AppendVarint64(value, out);
    }
    return out;
}

std::string RawValueBytes(const types::Variant& value) {
    std::string out;
    switch (value.type()) {
        case types::DataType::kNone:
            break;
        case types::DataType::kBool:
            out.push_back(value.as_bool() ? '\1' : '\0');
            break;
        case types::DataType::kInt8:
        case types::DataType::kInt16:
        case types::DataType::kInt32:
        case types::DataType::kInt64:
        case types::DataType::kTime:
            AppendVarint64(encode::Varints::zigzag_encode64(value.as_int()), out);
            break;
        case types::DataType::kUint8:
        case types::DataType::kUint16:
        case types::DataType::kUint32:
        case types::DataType::kUint64:
        case types::DataType::kVersion:
            AppendVarint64(value.as_uint(), out);
            break;
        case types::DataType::kString:
        case types::DataType::kU16String:
        case types::DataType::kU32String:
            out.append(value.as_string());
            break;
        case types::DataType::kBinary: {
            auto bytes = value.as_binary();
            out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            break;
        }
        default:
            value.encode_to(out);
            break;
    }
    return out;
}

uint64_t FlagFor(const types::Variant& value, bool embedded) {
    uint64_t flag = static_cast<uint64_t>(value.type());
    if (embedded) {
        flag |= kFlagEmbedded;
    }
    return flag;
}

std::string EncodeComparable(const types::Variant& value) {
    std::string out;
    out.push_back(static_cast<char>(value.type()));
    value.encode_to(out);
    return out;
}

std::string StatusTypeName(types::DataType type) {
    return std::string(types::data_type_name(type));
}

} // namespace

SSTableBuilder::SSTableBuilder(types::TableSchema schema,
                               std::string_view key_file_path,
                               Options opts)
    : schema_(std::move(schema)), opts_(std::move(opts)), key_file_path_(key_file_path) {}

absl::Status SSTableBuilder::validate_schema() const {
    if (schema_.row_key_columns().empty()) {
        return absl::InvalidArgumentError("SSTableV2 requires at least one row-key column");
    }
    for (const auto& column : schema_.row_key_columns()) {
        if (column.name.empty()) {
            return absl::InvalidArgumentError("row-key column name must not be empty");
        }
        if (column.type == types::DataType::kNone || types::is_private(column.type)) {
            return absl::InvalidArgumentError("invalid row-key column type: " +
                                              StatusTypeName(column.type));
        }
    }
    return absl::OkStatus();
}

absl::Status SSTableBuilder::validate_row(const types::Row& row) const {
    if (finished_ || aborted_) {
        return absl::FailedPreconditionError("builder is already finished or aborted");
    }
    if (auto status = validate_schema(); !status.ok()) {
        return status;
    }
    if (row.row_key.size() != schema_.num_row_key_columns()) {
        return absl::InvalidArgumentError("row-key column count does not match schema");
    }
    for (size_t i = 0; i < row.row_key.size(); ++i) {
        if (row.row_key[i].type() != schema_.row_key_column(i).type) {
            return absl::InvalidArgumentError("row-key column type mismatch");
        }
    }
    return absl::OkStatus();
}

std::string SSTableBuilder::encode_all_key(const types::Row& row) const {
    std::string out;
    for (const auto& key : row.row_key.values()) {
        out.append(EncodeComparable(key));
    }
    out.push_back(static_cast<char>(types::DataType::kVersion));
    AppendFixed64(UINT64_MAX - row.version, out); // descending version order.
    out.push_back(static_cast<char>(types::DataType::kUint8));
    out.push_back(static_cast<char>(row.op_type));
    return out;
}

absl::Status SSTableBuilder::append_value(types::Row row) {
    PendingRow pending;
    pending.row = std::move(row);
    pending.all_key = encode_all_key(pending.row);
    pending.value_bytes = RawValueBytes(pending.row.value);
    pending.value_length = pending.value_bytes.size();
    pending.value_checksum = Crc64(pending.value_bytes);
    bloom_keys_.push_back(pending.all_key);
    pending.embedded =
        pending.value_length <= opts_.max_embedded_value_size || opts_.value_file_path.empty();
    if (!pending.embedded) {
        pending.filename = opts_.value_file_path;
        pending.value_offset = 0;
        for (const auto& [_, length] : value_file_entries_) {
            pending.value_offset += length;
        }
        value_file_entries_.push_back({pending.value_bytes, pending.value_length});
    }
    data_size_ += pending.all_key.size() + pending.value_bytes.size();
    pending_rows_.push_back(std::move(pending));
    ++total_rows_;
    if (pending_rows_.size() > 1 &&
        pending_rows_.back().all_key <= pending_rows_[pending_rows_.size() - 2].all_key) {
        return absl::InvalidArgumentError(
            "rows must be added in strictly increasing all-key order");
    }
    if (pending_rows_.size() >= 2 &&
        build_data_block(std::span<const PendingRow>(pending_rows_)).size() >
            opts_.max_data_block_size) {
        PendingRow overflow = std::move(pending_rows_.back());
        pending_rows_.pop_back();
        --total_rows_;
        auto status = flush_data_block();
        ++total_rows_;
        pending_rows_.push_back(std::move(overflow));
        return status;
    }
    return absl::OkStatus();
}

absl::Status SSTableBuilder::add(const types::Row& row) {
    if (auto status = validate_row(row); !status.ok()) {
        return status;
    }
    return append_value(row);
}

absl::Status SSTableBuilder::add(std::string_view key, std::string_view value) {
    if (schema_.row_key_columns().empty()) {
        schema_ =
            types::TableSchema({types::KeyColumn{.name = "Key", .type = types::DataType::kString}});
    }
    types::Row row;
    row.row_key = types::StructuredRowKey({types::Variant::string(key)});
    row.value = types::Variant::string(value);
    return add(row);
}

std::string SSTableBuilder::build_data_block(std::span<const PendingRow> rows) const {
    std::string data_table;
    std::vector<std::pair<uint64_t, uint64_t>> all_key_refs;
    std::vector<uint64_t> versions;
    std::vector<uint64_t> op_types;
    std::vector<uint64_t> flags;
    std::vector<std::pair<uint64_t, uint64_t>> filename_refs;
    std::vector<uint64_t> value_offsets;
    std::vector<uint64_t> value_lengths;
    std::vector<uint64_t> value_checksums;
    std::vector<std::pair<uint64_t, uint64_t>> embedded_refs;

    for (const auto& row : rows) {
        auto append_data = [&](std::string_view bytes) {
            uint64_t offset = BlockHeader::kSize + data_table.size();
            data_table.append(bytes);
            return std::pair<uint64_t, uint64_t>{offset, bytes.size()};
        };
        all_key_refs.push_back(append_data(row.all_key));
        versions.push_back(row.row.version);
        op_types.push_back(row.row.op_type);
        flags.push_back(FlagFor(row.row.value, row.embedded));
        filename_refs.push_back(append_data(row.filename));
        value_offsets.push_back(row.embedded ? 0 : row.value_offset);
        value_lengths.push_back(row.value_length);
        value_checksums.push_back(row.value_checksum);
        embedded_refs.push_back(row.embedded ? append_data(row.value_bytes)
                                             : std::pair<uint64_t, uint64_t>{0, 0});
    }

    std::vector<std::string> units;
    units.push_back(EncodeVarlenUnit(all_key_refs));
    units.push_back(EncodeUintUnit(versions));
    units.push_back(EncodeUintUnit(op_types));
    units.push_back(EncodeUintUnit(flags));
    units.push_back(EncodeVarlenUnit(filename_refs));
    units.push_back(EncodeUintUnit(value_offsets));
    units.push_back(EncodeUintUnit(value_lengths));
    units.push_back(EncodeUintUnit(value_checksums));
    units.push_back(EncodeVarlenUnit(embedded_refs));
    return BuildBlock(kDataBlockMagic, rows.size(), std::move(data_table), std::move(units));
}

absl::Status SSTableBuilder::flush_data_block() {
    if (pending_rows_.empty()) {
        return absl::OkStatus();
    }
    std::string block = build_data_block(std::span<const PendingRow>(pending_rows_));
    data_blocks_.push_back(DataBlockIndex{
        .last_all_key = pending_rows_.back().all_key,
        .offset = key_file_.size(),
        .length = block.size(),
    });
    key_file_.append(block);
    pending_rows_.clear();
    return absl::OkStatus();
}

std::string SSTableBuilder::build_root_index_block() const {
    std::string data_table;
    std::vector<std::pair<uint64_t, uint64_t>> key_refs;
    std::vector<std::pair<uint64_t, uint64_t>> filename_refs;
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> lengths;

    for (const auto& entry : data_blocks_) {
        auto append_data = [&](std::string_view bytes) {
            uint64_t offset = BlockHeader::kSize + data_table.size();
            data_table.append(bytes);
            return std::pair<uint64_t, uint64_t>{offset, bytes.size()};
        };
        key_refs.push_back(append_data(entry.last_all_key));
        filename_refs.push_back(append_data("@2"));
        offsets.push_back(entry.offset);
        lengths.push_back(entry.length);
    }

    std::vector<std::string> units;
    units.push_back(EncodeVarlenUnit(key_refs));
    units.push_back(EncodeVarlenUnit(filename_refs));
    units.push_back(EncodeUintUnit(offsets));
    units.push_back(EncodeUintUnit(lengths));
    return BuildBlock(
        kRootBlockMagic, data_blocks_.size(), std::move(data_table), std::move(units));
}

std::string SSTableBuilder::build_bloom_filter() const {
    const uint64_t num_bits = std::max<uint64_t>(64, bloom_keys_.size() * opts_.bloom_bits_per_key);
    const uint64_t num_bytes = (num_bits + 7) / 8;
    constexpr uint32_t kNumHashes = 7;

    std::string out;
    AppendFixed32(kBloomMagic, out);
    AppendFixed32(kBloomVersion, out);
    AppendFixed32(kNumHashes, out);
    AppendFixed64(num_bits, out);
    AppendFixed64(bloom_keys_.size(), out);
    AppendFixed64(0, out); // checksum placeholder
    out.resize(out.size() + num_bytes, '\0');

    auto* bits = reinterpret_cast<unsigned char*>(out.data() + 36);
    for (const auto& key : bloom_keys_) {
        const uint64_t h1 = Hash64(key, 0x9e3779b97f4a7c15ULL);
        const uint64_t h2 = Hash64(key, 0xc2b2ae3d27d4eb4fULL) | 1ULL;
        for (uint32_t i = 0; i < kNumHashes; ++i) {
            const uint64_t bit = (h1 + i * h2) % num_bits;
            bits[bit / 8] |= static_cast<unsigned char>(1U << (bit % 8));
        }
    }

    std::fill(out.begin() + 28, out.begin() + 36, '\0');
    const uint64_t checksum = Crc64(out);
    encode::encode_fixed<uint64_t>(checksum, reinterpret_cast<std::byte*>(out.data()) + 28);
    return out;
}

std::string SSTableBuilder::build_metadata_section(
    uint32_t magic, const std::vector<std::pair<std::string, uint64_t>>& values) const {
    Locator map;
    for (const auto& [key, value] : values) {
        map.set_uint64(key, value);
    }
    std::string encoded = map.encode();
    encode::encode_fixed<uint32_t>(magic, reinterpret_cast<std::byte*>(encoded.data()));
    encode::encode_fixed<uint64_t>(0, reinterpret_cast<std::byte*>(encoded.data()) + 4);
    const uint64_t checksum = Crc64(encoded);
    encode::encode_fixed<uint64_t>(checksum, reinterpret_cast<std::byte*>(encoded.data()) + 4);
    return encoded;
}

std::string SSTableBuilder::build_schema_metadata() const {
    std::vector<std::pair<std::string, uint64_t>> values;
    values.push_back({"RowKeyColumnCount", schema_.num_row_key_columns()});
    for (size_t i = 0; i < schema_.num_row_key_columns(); ++i) {
        values.push_back({"RowKeyColumn" + std::to_string(i) + "_Type",
                          static_cast<uint64_t>(schema_.row_key_column(i).type)});
        values.push_back({"RowKeyColumn" + std::to_string(i) + "_Order",
                          static_cast<uint64_t>(schema_.row_key_column(i).order)});
    }
    return build_metadata_section(kSchemaMagic, values);
}

absl::Status SSTableBuilder::finish() {
    if (finished_) {
        return absl::FailedPreconditionError("builder already finished");
    }
    if (aborted_) {
        return absl::FailedPreconditionError("builder was aborted");
    }
    if (auto status = validate_schema(); !status.ok()) {
        return status;
    }
    if (auto status = flush_data_block(); !status.ok()) {
        return status;
    }

    if (!value_file_entries_.empty()) {
        std::ofstream value_file(opts_.value_file_path, std::ios::binary);
        if (!value_file) {
            return absl::InternalError("failed to create SSTableV2 value file");
        }
        for (const auto& [bytes, _] : value_file_entries_) {
            value_file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        if (!value_file) {
            return absl::InternalError("failed to write SSTableV2 value file");
        }
    }

    Locator locator;

    const uint64_t root_offset = key_file_.size();
    std::string root = build_root_index_block();
    key_file_.append(root);
    locator.set_section("RootIndex", SectionLocation{root_offset, root.size()});

    if (opts_.enable_bloom_filter) {
        const uint64_t bloom_offset = key_file_.size();
        std::string bloom = build_bloom_filter();
        key_file_.append(bloom);
        locator.set_section("BloomFilter0", SectionLocation{bloom_offset, bloom.size()});
    }

    const uint64_t configuration_offset = key_file_.size();
    std::string configuration =
        build_metadata_section(kConfigurationMagic,
                               {{"MaxEmbeddedValueSizeInByte", opts_.max_embedded_value_size},
                                {"MaxDataBlockSizeInByte_SoftLimit", opts_.max_data_block_size},
                                {"MaxDataBlockSizeInByte_HardLimit", opts_.max_data_block_size},
                                {"MaxDataBlockRowCount", total_rows_}});
    key_file_.append(configuration);
    locator.set_section("Configuration",
                        SectionLocation{configuration_offset, configuration.size()});

    const uint64_t schema_offset = key_file_.size();
    std::string schema = build_schema_metadata();
    key_file_.append(schema);
    locator.set_section("Schema", SectionLocation{schema_offset, schema.size()});

    const uint64_t statistics_offset = key_file_.size();
    std::string statistics = build_metadata_section(
        kStatisticsMagic,
        {{"TotalRowCount", total_rows_}, {"DataBlockCount", data_blocks_.size()}});
    key_file_.append(statistics);
    locator.set_section("Statistics", SectionLocation{statistics_offset, statistics.size()});

    if (opts_.emit_compatibility_metadata) {
        const uint64_t compatibility_offset = key_file_.size();
        std::string compatibility = build_metadata_section(kCompatibilityMagic,
                                                           {{"UInt64_Size", sizeof(uint64_t)},
                                                            {"UInt32_Size", sizeof(uint32_t)},
                                                            {"UInt8_Size", sizeof(uint8_t)}});
        key_file_.append(compatibility);
        locator.set_section("Compatibility",
                            SectionLocation{compatibility_offset, compatibility.size()});
    }

    std::string locator_bytes = locator.encode();
    Tail tail;
    tail.locator_offset = key_file_.size();
    tail.locator_length = locator_bytes.size();
    key_file_.append(locator_bytes);
    auto tail_bytes = tail.encode();
    key_file_.append(reinterpret_cast<const char*>(tail_bytes.data()), tail_bytes.size());

    std::ofstream out(key_file_path_, std::ios::binary);
    if (!out) {
        return absl::InternalError("failed to create SSTableV2 key file");
    }
    out.write(key_file_.data(), static_cast<std::streamsize>(key_file_.size()));
    if (!out) {
        return absl::InternalError("failed to write SSTableV2 key file");
    }
    finished_ = true;
    return absl::OkStatus();
}

void SSTableBuilder::abort() {
    aborted_ = true;
}

uint64_t SSTableBuilder::total_rows() const {
    return total_rows_;
}

uint64_t SSTableBuilder::data_size() const {
    return data_size_;
}

} // namespace pl::sstv2::file
