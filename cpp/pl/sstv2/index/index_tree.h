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
// Created: 2026/06/07 00:00

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/compress/compress.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"

namespace pl::sstv2::index {

struct BlockRef {
    uint64_t offset = 0;
    uint64_t length = 0;
};

struct FinishResult {
    BlockRef root;
    uint64_t block_count = 0;
};

class TreeBuilder {
public:
    TreeBuilder(types::InternalSchema::ConstRef schema,
                uint64_t fanout,
                uint64_t soft_limit,
                uint64_t hard_limit,
                compress::Options compression,
                std::string* key_file);

    [[nodiscard]] absl::Status prepare_for_data_block();
    [[nodiscard]] absl::Status add_data_block(const types::InternalRow& fence,
                                              BlockRef data_block,
                                              uint64_t row_count);
    [[nodiscard]] absl::StatusOr<FinishResult> finish();

private:
    [[nodiscard]] absl::Status add_index_entry(size_t level, types::InternalRow entry);
    [[nodiscard]] absl::Status flush_index_level(size_t level);

    types::InternalSchema::ConstRef schema_;
    uint64_t fanout_ = 2;
    uint64_t soft_limit_ = 64 * 1024;
    uint64_t hard_limit_ = 128 * 1024;
    compress::Options compression_;
    std::string* key_file_ = nullptr;
    std::vector<std::vector<types::InternalRow>> levels_;
    uint64_t block_count_ = 0;
};

class TreeReader {
public:
    [[nodiscard]] static absl::Status scan_data_blocks(std::string_view key_file,
                                                       types::InternalSchema::ConstRef schema,
                                                       BlockRef root,
                                                       std::vector<BlockRef>* data_blocks);

    [[nodiscard]] static absl::StatusOr<std::optional<BlockRef>> find_data_block(
        std::string_view key_file,
        types::InternalSchema::ConstRef schema,
        BlockRef root,
        std::string_view target_all_key);
};

} // namespace pl::sstv2::index
