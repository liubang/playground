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
// Created: 2026/05/17 22:43

#include "cpp/pl/sst/block.h"
#include "cpp/pl/sst/block_builder.h"
#include "cpp/pl/sst/cell.h"
#include "cpp/pl/sst/comparator.h"
#include "cpp/pl/sst/options.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace pl {

class BlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        options_ = std::make_shared<BuildOptions>();
        options_->block_restart_interval = 4;
    }

    // Helper: build a block using a lambda that adds cells to the builder.
    // Cell holds string_view, so the underlying data must be alive during add().
    // By using a lambda, callers can keep strings on stack during builder.add().
    template <typename Func> BlockRef buildBlockWith(Func&& add_cells) {
        BlockBuilder builder(options_);
        add_cells(builder);
        auto data = builder.finish();
        block_data_ = std::string(data);
        BlockContents contents;
        contents.data = block_data_;
        contents.heap_allocated = false;
        contents.cachable = false;
        return std::make_shared<Block>(contents);
    }

    ComparatorRef comparator_ = std::make_shared<BytewiseComparator>();
    BuildOptionsRef options_;
    std::string block_data_;
};

// ==================== Basic Iterator Tests ====================

TEST_F(BlockTest, empty_block_iterator) {
    auto block = buildBlockWith([](BlockBuilder& /*builder*/) {
        // add nothing
    });

    auto iter = block->iterator(comparator_);
    iter->first();
    EXPECT_FALSE(iter->valid());
}

TEST_F(BlockTest, single_entry) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell cell(CellType::CT_PUT, "row1", "cf", "col1", "value1", 1000);
        builder.add(cell);
    });

    auto iter = block->iterator(comparator_);
    iter->first();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row1");
    EXPECT_EQ(iter->cell()->cf(), "cf");
    EXPECT_EQ(iter->cell()->col(), "col1");
    EXPECT_EQ(iter->cell()->value(), "value1");
    EXPECT_EQ(iter->cell()->timestamp(), 1000u);
    EXPECT_EQ(iter->cell()->cell_type(), CellType::CT_PUT);

    iter->next();
    EXPECT_FALSE(iter->valid());
}

TEST_F(BlockTest, multiple_entries_forward_iteration) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 20; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "row_%03d", i);
            std::string rowkey(buf);
            std::string val = "val_" + std::to_string(i);
            Cell cell(CellType::CT_PUT, rowkey, "cf", "col", val, 1000 + i);
            builder.add(cell);
        }
    });

    auto iter = block->iterator(comparator_);
    iter->first();
    int count = 0;
    while (iter->valid()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "row_%03d", count);
        EXPECT_EQ(iter->cell()->rowkey(), std::string(buf));
        iter->next();
        count++;
    }
    EXPECT_EQ(count, 20);
}

TEST_F(BlockTest, last_entry) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 10; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "row_%03d", i);
            std::string rowkey(buf);
            Cell cell(CellType::CT_PUT, rowkey, "cf", "col", "val", 1000 + i);
            builder.add(cell);
        }
    });

    auto iter = block->iterator(comparator_);
    iter->last();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_009");
}

TEST_F(BlockTest, prev_from_last) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 5; ++i) {
            std::string rowkey = "row_" + std::to_string(i);
            Cell cell(CellType::CT_PUT, rowkey, "cf", "col", "val", 1000 + i);
            builder.add(cell);
        }
    });

    auto iter = block->iterator(comparator_);
    iter->last();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_4");

    iter->prev();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_3");

    iter->prev();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_2");

    iter->prev();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_1");

    iter->prev();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "row_0");

    iter->prev();
    EXPECT_FALSE(iter->valid());
}

// ==================== Seek Tests ====================

TEST_F(BlockTest, seek_exact_match) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 10; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "key_%03d", i);
            std::string rowkey(buf);
            Cell cell(CellType::CT_PUT, rowkey, "cf", "col", "val", 1000);
            builder.add(cell);
        }
    });

    auto iter = block->iterator(comparator_);
    iter->seek("key_005");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "key_005");
}

TEST_F(BlockTest, seek_between_keys) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell c1(CellType::CT_PUT, "aaa", "cf", "col", "v", 1000);
        builder.add(c1);
        Cell c2(CellType::CT_PUT, "ccc", "cf", "col", "v", 1000);
        builder.add(c2);
        Cell c3(CellType::CT_PUT, "eee", "cf", "col", "v", 1000);
        builder.add(c3);
        Cell c4(CellType::CT_PUT, "ggg", "cf", "col", "v", 1000);
        builder.add(c4);
    });

    auto iter = block->iterator(comparator_);
    // Seek to "bbb" -> should land on "ccc" (first key >= "bbb")
    iter->seek("bbb");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "ccc");
}

TEST_F(BlockTest, seek_before_first) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell c1(CellType::CT_PUT, "bbb", "cf", "col", "v", 1000);
        builder.add(c1);
        Cell c2(CellType::CT_PUT, "ccc", "cf", "col", "v", 1000);
        builder.add(c2);
    });

    auto iter = block->iterator(comparator_);
    iter->seek("aaa");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "bbb");
}

TEST_F(BlockTest, seek_past_last) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell c1(CellType::CT_PUT, "aaa", "cf", "col", "v", 1000);
        builder.add(c1);
        Cell c2(CellType::CT_PUT, "bbb", "cf", "col", "v", 1000);
        builder.add(c2);
    });

    auto iter = block->iterator(comparator_);
    iter->seek("zzz");
    EXPECT_FALSE(iter->valid());
}

// ==================== Prefix Compression Tests ====================

TEST_F(BlockTest, prefix_compression_with_shared_prefix) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 20; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "common_prefix_row_%03d", i);
            std::string rowkey(buf);
            std::string val = "value_" + std::to_string(i);
            Cell cell(CellType::CT_PUT, rowkey, "default", "column_a", val, 5000 + i);
            builder.add(cell);
        }
    });

    // Verify forward iteration
    auto iter = block->iterator(comparator_);
    iter->first();
    int count = 0;
    while (iter->valid()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "common_prefix_row_%03d", count);
        EXPECT_EQ(iter->cell()->rowkey(), std::string(buf));
        EXPECT_EQ(iter->cell()->value(), "value_" + std::to_string(count));
        iter->next();
        count++;
    }
    EXPECT_EQ(count, 20);

    // Verify seek within prefix-compressed block
    iter->seek("common_prefix_row_010");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "common_prefix_row_010");
}

// ==================== Restart Point Tests ====================

TEST_F(BlockTest, restart_points_boundary) {
    options_->block_restart_interval = 4;
    auto block = buildBlockWith([](BlockBuilder& builder) {
        for (int i = 0; i < 12; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "r_%03d", i);
            std::string rowkey(buf);
            Cell cell(CellType::CT_PUT, rowkey, "cf", "c", "v", 1000);
            builder.add(cell);
        }
    });

    auto iter = block->iterator(comparator_);
    iter->seek("r_004");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "r_004");

    iter->seek("r_008");
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "r_008");
}

// ==================== Invalid Block Tests ====================

TEST_F(BlockTest, invalid_block_too_many_restarts) {
    // Craft a block with num_restarts > max possible
    std::string data(8, '\0');
    uint32_t fake_restarts = 999999;
    std::memcpy(data.data() + data.size() - 4, &fake_restarts, 4);

    BlockContents contents;
    contents.data = data;
    contents.heap_allocated = false;
    contents.cachable = false;
    Block block(contents);
    EXPECT_FALSE(block.valid());
}

TEST_F(BlockTest, block_move_semantics) {
    BlockBuilder builder(options_);
    {
        Cell cell(CellType::CT_PUT, "key1", "cf", "col", "val1", 1000);
        builder.add(cell);
    }
    {
        Cell cell(CellType::CT_PUT, "key2", "cf", "col", "val2", 2000);
        builder.add(cell);
    }
    auto data = builder.finish();

    auto* buf = new char[data.size()];
    std::memcpy(buf, data.data(), data.size());
    BlockContents contents;
    contents.data = std::string_view(buf, data.size());
    contents.heap_allocated = true;
    contents.cachable = true;

    auto block1 = std::make_shared<Block>(contents);
    EXPECT_TRUE(block1->valid());

    Block block2(std::move(*block1));
    EXPECT_TRUE(block2.valid());
    EXPECT_FALSE(block1->valid());
}

// ==================== CellType Preservation ====================

TEST_F(BlockTest, cell_types_preserved) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell c1(CellType::CT_PUT, "row_0", "cf", "col", "val", 1000);
        builder.add(c1);
        Cell c2(CellType::CT_DEL, "row_1", "cf", "col", "", 2000);
        builder.add(c2);
        Cell c3(CellType::CT_READ, "row_2", "cf", "col", "val", 3000);
        builder.add(c3);
    });

    auto iter = block->iterator(comparator_);
    iter->first();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->cell_type(), CellType::CT_PUT);

    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->cell_type(), CellType::CT_DEL);

    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->cell_type(), CellType::CT_READ);

    iter->next();
    EXPECT_FALSE(iter->valid());
}

// ==================== Multiple Column Families ====================

TEST_F(BlockTest, multiple_cf_and_columns) {
    auto block = buildBlockWith([](BlockBuilder& builder) {
        Cell c1(CellType::CT_PUT, "user_001", "info", "name", "Alice", 1000);
        builder.add(c1);
        Cell c2(CellType::CT_PUT, "user_001", "info", "age", "30", 1000);
        builder.add(c2);
        Cell c3(CellType::CT_PUT, "user_001", "stats", "login_count", "42", 1000);
        builder.add(c3);
        Cell c4(CellType::CT_PUT, "user_002", "info", "name", "Bob", 1000);
        builder.add(c4);
    });

    auto iter = block->iterator(comparator_);
    iter->first();

    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "user_001");
    EXPECT_EQ(iter->cell()->cf(), "info");
    EXPECT_EQ(iter->cell()->col(), "name");
    EXPECT_EQ(iter->cell()->value(), "Alice");

    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "user_001");
    EXPECT_EQ(iter->cell()->cf(), "info");
    EXPECT_EQ(iter->cell()->col(), "age");

    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "user_001");
    EXPECT_EQ(iter->cell()->cf(), "stats");
    EXPECT_EQ(iter->cell()->col(), "login_count");

    iter->next();
    ASSERT_TRUE(iter->valid());
    EXPECT_EQ(iter->cell()->rowkey(), "user_002");
}

} // namespace pl
