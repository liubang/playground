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

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/index/index_tree.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/key_factory.h"
#include "cpp/pl/sstv2/types/row.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::index {
namespace {

using types::DataType;
using types::InternalRow;
using types::InternalSchema;
using types::OpType;
using types::Schema;
using types::SchemaBuilder;
using types::Value;
using types::Version;

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder().add_column("id", DataType::kUint64).build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

InternalRow make_fence_row(const InternalSchema::ConstRef& schema, uint64_t id) {
    InternalRow row = InternalRow::make(schema);
    row.columns[0] = Value::make<DataType::kUint64>(id);
    row.columns[schema->version_index()] =
        Value::make<DataType::kVersion>(Version{.major = 10, .minor = id});
    row.columns[schema->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema->flag_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->filename_index()] = Value::make<DataType::kString>("@1");
    row.columns[schema->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->length_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    return row;
}

types::AllKey all_key_for(const InternalSchema::ConstRef& schema, const InternalRow& row) {
    auto all_key = types::make_all_key(row, schema);
    EXPECT_TRUE(all_key.ok()) << all_key.status();
    return std::move(*all_key);
}

types::PrefixKey prefix_key_for(const InternalSchema::ConstRef& schema, uint64_t id) {
    auto prefix =
        types::make_prefix_key(types::KeyPrefix{.key_columns = {Value::make<DataType::kUint64>(id)},
                                                .version = std::nullopt,
                                                .op_type = std::nullopt},
                               schema->user_schema(),
                               schema);
    EXPECT_TRUE(prefix.ok()) << prefix.status();
    return std::move(*prefix);
}

uint64_t block_length(std::string_view key_file, uint64_t offset) {
    const uint64_t uncompressed = codec::read_fixed64(key_file, offset + 28);
    const uint64_t compressed = codec::read_fixed64(key_file, offset + 36);
    return compressed == 0 ? uncompressed : compressed;
}

TEST(IndexTreeTest, BuildsPostOrderTreeAndRoutesToDataBlock) {
    auto schema = InternalSchema::make(make_schema());
    std::string key_file;
    TreeBuilder builder(schema, 2, 4096, 8192, {}, &key_file);

    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->block_count, 3u);

    const uint64_t first_index = 0;
    const uint64_t second_index = block_length(key_file, first_index);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_file, first_index)),
              block::Kind::kIndex);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_file, second_index)),
              block::Kind::kIndex);
    EXPECT_EQ(static_cast<block::Kind>(codec::read_fixed32(key_file, result->root.offset)),
              block::Kind::kRootIndex);

    std::vector<BlockRef> refs;
    ASSERT_TRUE(TreeReader::scan_data_blocks(key_file, schema, result->root, &refs).ok());
    ASSERT_EQ(refs.size(), 4u);
    EXPECT_EQ(refs[0].offset, 0u);
    EXPECT_EQ(refs[3].offset, 30u);

    auto found = TreeReader::find_data_block(
        key_file, schema, result->root, all_key_for(schema, make_fence_row(schema, 2)));
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->offset, 20u);
    EXPECT_EQ((*found)->length, 10u);
}

TEST(IndexTreeTest, RangeScanPrunesBlocksAfterLimitFence) {
    auto schema = InternalSchema::make(make_schema());
    std::string key_file;
    TreeBuilder builder(schema, 2, 4096, 8192, {}, &key_file);

    for (uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(builder.prepare_for_data_block().ok());
        ASSERT_TRUE(
            builder.add_data_block(make_fence_row(schema, i), BlockRef{i * 10, 10}, 2).ok());
    }

    auto result = builder.finish();
    ASSERT_TRUE(result.ok()) << result.status();

    std::vector<BlockRef> refs;
    const auto limit = prefix_key_for(schema, 2);
    ASSERT_TRUE(TreeReader::scan_data_blocks_in_range(key_file,
                                                      schema,
                                                      result->root,
                                                      std::nullopt,
                                                      std::optional<types::PrefixKey>{limit},
                                                      &refs)
                    .ok());
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].offset, 0u);
    EXPECT_EQ(refs[1].offset, 10u);
    EXPECT_EQ(refs[2].offset, 20u);

    refs.clear();
    const auto start = prefix_key_for(schema, 1);
    ASSERT_TRUE(TreeReader::scan_data_blocks_in_range(key_file,
                                                      schema,
                                                      result->root,
                                                      std::optional<types::PrefixKey>{start},
                                                      std::optional<types::PrefixKey>{limit},
                                                      &refs)
                    .ok());
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].offset, 10u);
    EXPECT_EQ(refs[1].offset, 20u);
}

} // namespace
} // namespace pl::sstv2::index
