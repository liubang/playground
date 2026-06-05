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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/compress/compress.h"
#include "cpp/pl/sstv2/types/internal_row.h"

namespace pl::sstv2::block {

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
};

class BlockBuilder {
public:
    BlockBuilder(types::InternalSchema::ConstPtr schema, Options options);

    [[nodiscard]] absl::Status add(types::InternalRow row);
    [[nodiscard]] absl::StatusOr<std::string> finish() const;

    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }

private:
    types::InternalSchema::ConstPtr schema_;
    Options options_;
    std::vector<types::InternalRow> rows_;
};

class BlockReader {
public:
    [[nodiscard]] static absl::StatusOr<BlockReader> open(std::string_view block,
                                                          const types::InternalSchema& schema,
                                                          Kind expected);

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<types::InternalRow>& rows() const noexcept { return rows_; }

private:
    Header header_;
    std::vector<types::InternalRow> rows_;
};

} // namespace pl::sstv2::block
