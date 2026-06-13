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

#include "cpp/pl/sstv2/index/index_tree.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/types/column_flag.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::index {
namespace {

using types::ColumnFlag;
using types::DataType;
using types::InternalRow;
using types::InternalSchema;
using types::Value;

absl::StatusOr<std::string_view> checked_slice(std::string_view bytes,
                                               uint64_t offset,
                                               uint64_t length,
                                               std::string_view label) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        return absl::InvalidArgumentError(absl::StrCat(label, " points outside file"));
    }
    return bytes.substr(static_cast<size_t>(offset), static_cast<size_t>(length));
}

absl::StatusOr<types::AllKeyView> all_key_for(InternalSchema::ConstRef schema,
                                              const InternalRow& row) {
    return types::make_all_key_view(row, std::move(schema));
}

template <typename KeyTag>
absl::StatusOr<size_t> lower_bound_by_key(const InternalSchema::ConstRef& schema,
                                          const std::vector<InternalRow>& rows,
                                          const types::LogicalKey<KeyTag>& target_key) {
    types::KeyComparator comparator(schema);
    size_t first = 0;
    size_t count = rows.size();
    while (count > 0) {
        const size_t step = count / 2;
        const size_t it = first + step;
        auto key = all_key_for(schema, rows[it]);
        if (!key.ok())
            return key.status();
        absl::StatusOr<bool> less;
        if constexpr (std::is_same_v<KeyTag, types::PrefixKeyTag>) {
            less = comparator.all_key_less_than_prefix(*key, target_key);
        } else {
            less = comparator.all_key_less(*key, target_key);
        }
        if (!less.ok())
            return less.status();
        if (*less) {
            first = it + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

InternalRow make_index_row(const InternalSchema::ConstRef& schema,
                           const InternalRow& fence,
                           BlockRef block,
                           uint64_t subtree_row_count,
                           ColumnFlag pointer_flag) {
    InternalRow index = InternalRow::make(schema);
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        index.columns[i] = fence.columns[i];
    }
    index.columns[schema->flag_index()] = Value::make<DataType::kUint64>(pointer_flag.raw());
    index.columns[schema->filename_index()] =
        Value::make<DataType::kString>(types::kKeyFileFilename);
    index.columns[schema->offset_index()] = Value::make<DataType::kUint64>(block.offset);
    index.columns[schema->length_index()] = Value::make<DataType::kUint64>(block.length);
    index.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(subtree_row_count);
    return index;
}

uint64_t subtree_row_count(const InternalSchema::ConstRef& schema,
                           const std::vector<InternalRow>& rows) {
    uint64_t count = 0;
    for (const auto& row : rows) {
        count += row.checksum(schema);
    }
    return count;
}

absl::StatusOr<block::BlockReader> open_index_node(std::string_view key_file,
                                                   const InternalSchema::ConstRef& schema,
                                                   BlockRef ref,
                                                   block::Kind kind) {
    auto bytes = checked_slice(key_file, ref.offset, ref.length, "index node");
    if (!bytes.ok())
        return bytes.status();
    return block::BlockReader::open(*bytes, schema, kind);
}

absl::Status scan_node(std::string_view key_file,
                       const InternalSchema::ConstRef& schema,
                       BlockRef ref,
                       block::Kind kind,
                       std::vector<BlockRef>* data_blocks) {
    auto node = open_index_node(key_file, schema, ref, kind);
    if (!node.ok())
        return node.status();

    for (const auto& entry : node->rows()) {
        const ColumnFlag flag = entry.flag(schema);
        const BlockRef child{.offset = entry.offset(schema), .length = entry.length(schema)};
        if (flag.is_data_block_ptr()) {
            data_blocks->push_back(child);
        } else if (flag.is_index_block_ptr()) {
            auto status = scan_node(key_file, schema, child, block::Kind::kIndex, data_blocks);
            if (!status.ok())
                return status;
        } else {
            return absl::InvalidArgumentError("index node contains non-index entry");
        }
    }
    return absl::OkStatus();
}

absl::Status scan_node_range(std::string_view key_file,
                             const InternalSchema::ConstRef& schema,
                             BlockRef ref,
                             block::Kind kind,
                             const std::optional<types::PrefixKey>& start_key,
                             const std::optional<types::PrefixKey>& limit_key,
                             std::vector<BlockRef>* data_blocks) {
    auto node = open_index_node(key_file, schema, ref, kind);
    if (!node.ok())
        return node.status();

    types::KeyComparator comparator(schema);
    for (const auto& entry : node->rows()) {
        auto fence = all_key_for(schema, entry);
        if (!fence.ok())
            return fence.status();
        if (start_key.has_value()) {
            auto less = comparator.all_key_less_than_prefix(*fence, *start_key);
            if (!less.ok())
                return less.status();
            if (*less) {
                continue;
            }
        }

        bool stop_after_child = false;
        if (limit_key.has_value()) {
            auto cmp = comparator.compare_all_key_to_prefix(*fence, *limit_key);
            if (!cmp.ok())
                return cmp.status();
            stop_after_child = *cmp >= 0;
        }

        const ColumnFlag flag = entry.flag(schema);
        const BlockRef child{.offset = entry.offset(schema), .length = entry.length(schema)};
        if (flag.is_data_block_ptr()) {
            data_blocks->push_back(child);
        } else if (flag.is_index_block_ptr()) {
            auto status = scan_node_range(
                key_file, schema, child, block::Kind::kIndex, start_key, limit_key, data_blocks);
            if (!status.ok())
                return status;
        } else {
            return absl::InvalidArgumentError("index node contains non-index entry");
        }
        if (stop_after_child) {
            break;
        }
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<BlockRef>> find_data_block_from_node(
    std::string_view key_file,
    const InternalSchema::ConstRef& schema,
    BlockRef ref,
    block::Kind kind,
    const types::AllKey& target_key) {
    auto node = open_index_node(key_file, schema, ref, kind);
    if (!node.ok())
        return node.status();

    auto selected_index = lower_bound_by_key(schema, node->rows(), target_key);
    if (!selected_index.ok())
        return selected_index.status();
    if (*selected_index == node->rows().size()) {
        return std::optional<BlockRef>{};
    }
    const InternalRow& selected = node->rows()[*selected_index];
    const ColumnFlag flag = selected.flag(schema);
    const BlockRef child{.offset = selected.offset(schema), .length = selected.length(schema)};
    if (flag.is_data_block_ptr()) {
        return std::optional<BlockRef>{child};
    }
    if (flag.is_index_block_ptr()) {
        return find_data_block_from_node(key_file, schema, child, block::Kind::kIndex, target_key);
    }
    return absl::InvalidArgumentError("index node contains non-index entry");
}

} // namespace

TreeBuilder::TreeBuilder(types::InternalSchema::ConstRef schema,
                         uint64_t fanout,
                         uint64_t soft_limit,
                         uint64_t hard_limit,
                         compress::Options compression,
                         std::string* key_file)
    : schema_(std::move(schema)),
      fanout_(std::max<uint64_t>(2, fanout)),
      soft_limit_(std::max<uint64_t>(block::Header::kSize, soft_limit)),
      hard_limit_(std::max<uint64_t>(soft_limit_, hard_limit)),
      compression_(compression),
      key_file_(key_file) {}

absl::Status TreeBuilder::prepare_for_data_block() {
    if (schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (key_file_ == nullptr) {
        return absl::InvalidArgumentError("key file is null");
    }
    if (!levels_.empty() && levels_[0].size() == fanout_) {
        auto status = flush_index_level(0);
        if (!status.ok())
            return status;
    }
    return absl::OkStatus();
}

absl::Status TreeBuilder::add_data_block(const InternalRow& fence,
                                         BlockRef data_block,
                                         uint64_t row_count) {
    if (schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (key_file_ == nullptr) {
        return absl::InvalidArgumentError("key file is null");
    }
    if (!levels_.empty() && levels_[0].size() == fanout_) {
        return absl::FailedPreconditionError(
            "prepare_for_data_block must be called before adding another data block");
    }
    return add_index_entry(
        0, make_index_row(schema_, fence, data_block, row_count, ColumnFlag::for_data_block()));
}

absl::Status TreeBuilder::add_index_entry(size_t level, InternalRow entry) {
    if (levels_.size() <= level) {
        levels_.resize(level + 1);
    }
    if (level > 0 && levels_[level].size() == fanout_) {
        auto status = flush_index_level(level);
        if (!status.ok())
            return status;
    }
    levels_[level].push_back(std::move(entry));
    return absl::OkStatus();
}

absl::Status TreeBuilder::flush_index_level(size_t level) {
    if (level >= levels_.size() || levels_[level].empty()) {
        return absl::OkStatus();
    }

    block::Options options;
    options.kind = block::Kind::kIndex;
    options.compression = compression_;
    options.max_block_size_soft_limit = soft_limit_;
    options.max_block_size_hard_limit = hard_limit_;
    options.max_row_count = fanout_;
    block::BlockBuilder builder(schema_, options);
    auto parent = make_index_row(schema_,
                                 levels_[level].back(),
                                 BlockRef{.offset = key_file_->size(), .length = 0},
                                 subtree_row_count(schema_, levels_[level]),
                                 ColumnFlag::for_index_block());
    for (auto& entry : levels_[level]) {
        auto status = builder.add(std::move(entry));
        if (!status.ok())
            return status;
    }
    auto encoded = builder.finish();
    if (!encoded.ok())
        return encoded.status();

    const BlockRef ref{.offset = key_file_->size(), .length = encoded->size()};
    key_file_->append(*encoded);
    ++block_count_;
    parent.columns[schema_->offset_index()] = Value::make<DataType::kUint64>(ref.offset);
    parent.columns[schema_->length_index()] = Value::make<DataType::kUint64>(ref.length);
    levels_[level].clear();
    return add_index_entry(level + 1, std::move(parent));
}

absl::StatusOr<FinishResult> TreeBuilder::finish() {
    if (schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (key_file_ == nullptr) {
        return absl::InvalidArgumentError("key file is null");
    }

    while (true) {
        size_t non_empty_count = 0;
        size_t only_non_empty_level = 0;
        size_t lowest_non_empty_level = levels_.size();
        for (size_t level = 0; level < levels_.size(); ++level) {
            if (levels_[level].empty()) {
                continue;
            }
            ++non_empty_count;
            only_non_empty_level = level;
            lowest_non_empty_level = std::min(lowest_non_empty_level, level);
        }
        if (non_empty_count <= 1) {
            std::vector<InternalRow> root_entries;
            if (non_empty_count == 1) {
                root_entries = std::move(levels_[only_non_empty_level]);
                levels_[only_non_empty_level].clear();
            }
            block::Options options;
            options.kind = block::Kind::kRootIndex;
            options.compression = compression_;
            options.max_block_size_soft_limit = soft_limit_;
            options.max_block_size_hard_limit = hard_limit_;
            options.max_row_count = fanout_;
            block::BlockBuilder root_builder(schema_, options);
            for (auto& entry : root_entries) {
                auto status = root_builder.add(std::move(entry));
                if (!status.ok())
                    return status;
            }
            auto root = root_builder.finish();
            if (!root.ok())
                return root.status();

            const BlockRef root_ref{.offset = key_file_->size(), .length = root->size()};
            key_file_->append(*root);
            ++block_count_;
            return FinishResult{.root = root_ref, .block_count = block_count_};
        }
        auto status = flush_index_level(lowest_non_empty_level);
        if (!status.ok())
            return status;
    }
}

absl::Status TreeReader::scan_data_blocks(std::string_view key_file,
                                          const InternalSchema::ConstRef& schema,
                                          BlockRef root,
                                          std::vector<BlockRef>* data_blocks) {
    if (data_blocks == nullptr) {
        return absl::InvalidArgumentError("data blocks output is null");
    }
    return scan_node(key_file, schema, root, block::Kind::kRootIndex, data_blocks);
}

absl::Status TreeReader::scan_data_blocks_from(std::string_view key_file,
                                               const InternalSchema::ConstRef& schema,
                                               BlockRef root,
                                               const types::PrefixKey& start_key,
                                               std::vector<BlockRef>* data_blocks) {
    return scan_data_blocks_in_range(key_file, schema, root, start_key, std::nullopt, data_blocks);
}

absl::Status TreeReader::scan_data_blocks_in_range(std::string_view key_file,
                                                   const InternalSchema::ConstRef& schema,
                                                   BlockRef root,
                                                   const std::optional<types::PrefixKey>& start_key,
                                                   const std::optional<types::PrefixKey>& limit_key,
                                                   std::vector<BlockRef>* data_blocks) {
    if (data_blocks == nullptr) {
        return absl::InvalidArgumentError("data blocks output is null");
    }
    return scan_node_range(
        key_file, schema, root, block::Kind::kRootIndex, start_key, limit_key, data_blocks);
}

absl::StatusOr<std::optional<BlockRef>> TreeReader::find_data_block(
    std::string_view key_file,
    const InternalSchema::ConstRef& schema,
    BlockRef root,
    const types::AllKey& target_key) {
    return find_data_block_from_node(key_file, schema, root, block::Kind::kRootIndex, target_key);
}

} // namespace pl::sstv2::index
