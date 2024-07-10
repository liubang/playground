// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/log/logger.h"
#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/sstable.h"
#include "cpp/pl/sst/sstable_builder.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <set>

namespace pl {

struct CaseCell {
    std::string rowkey;
    std::string cf;
    std::string col;
    CellType type;
    uint64_t ts;
    std::string val;
    [[nodiscard]] Cell to_cell() const { return {type, rowkey, cf, col, val, ts}; }
};

struct CaseCellComparator {
    bool operator()(const CaseCell& lhs, const CaseCell& rhs) const {
        if (lhs.rowkey != rhs.rowkey) {
            return lhs.rowkey < rhs.rowkey;
        }
        if (lhs.cf != rhs.cf) {
            return lhs.cf < rhs.cf;
        }
        if (lhs.col != rhs.col) {
            return lhs.col < rhs.col;
        }
        if (lhs.ts != rhs.ts) {
            return lhs.ts > rhs.ts;
        }
        if (lhs.type != rhs.type) {
            return lhs.type < rhs.type;
        }
        return false;
    }
};

namespace {
std::vector<std::string> sst_files = {"/tmp/MAJOR/1.sst", "/tmp/MAJOR/2.sst", "/tmp/MAJOR/3.sst"};
std::vector<std::set<CaseCell, CaseCellComparator>> cellses =
    std::vector<std::set<CaseCell, CaseCellComparator>>(3);
} // namespace

class SSTableTest : public ::testing::Test {
    void SetUp() override { read_options = std::make_shared<ReadOptions>(); }

    void TearDown() override {}

public:
    BuildOptionsRef new_build_options() {
        auto build_options = std::make_shared<BuildOptions>();
        build_options->data_dir = "/tmp";
        build_options->compression_type = CompressionType::NONE;
        build_options->sst_type = SSTType::MAJOR;
        build_options->sst_version = SSTVersion::V1;
        build_options->filter_type = FilterPolicyType::BLOOM_FILTER;
        return build_options;
    }

    void build_sst(int idx, const BuildOptionsRef& build_options) {
        auto sst_file = sst_files[idx];
        auto& cells = cellses[idx];

        auto sstable_builder = std::make_unique<pl::SSTableBuilder>(build_options);
        auto st = sstable_builder->open();
        if (!st.ok()) {
            LOG(ERROR) << "error: " << (int)st.code() << ", message: " << st.msg();
        }
        EXPECT_EQ(Code::ST_Ok, st.code());
        for (int i = 0; i < ROW_NUM; ++i) {
            std::string rowkey =
                pl::random_string(ROWKEY_LEN + (i % ROWKEY_LEN)) + std::to_string(i);
            for (int j = 0; j < COL_NUM; ++j) {
                for (int k = 0; k < 2; ++k) {
                    std::string col = pl::random_string(COL_LEN);
                    std::string val = pl::random_string(VAL_LEN);
                    CaseCell c = CaseCell{
                        .rowkey = rowkey,
                        .cf = CF1,
                        .col = col,
                        .type = static_cast<CellType>(i % 3),
                        .ts = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()),
                        .val = val,
                    };
                    if (k == 1) {
                        c.cf = CF2;
                    }
                    cells.insert(c);
                }
            }
        }

        for (const auto& cell : cells) {
            sstable_builder->add(cell.to_cell());
        }
        auto status = sstable_builder->finish();
        EXPECT_TRUE(status.isOk());
    }

    void seek_from_sst(int idx) {
        auto sst_file = sst_files[idx];
        auto cells = cellses[idx];
        auto table = pl::SSTable::open(read_options, sst_file, &st);
        EXPECT_TRUE(st.isOk());

        check_table(table.get());

        auto it = table->iterator();
        for (const auto& cell : cells) {
            it->seek(cell.rowkey);
            EXPECT_TRUE(it->valid());
            auto c = it->cell();
            EXPECT_TRUE(c != nullptr);
            EXPECT_EQ(cell.rowkey, c->rowkey());
        }
    }

    void check_table(SSTable* table) {
        LOG(INFO) << "======== sst file meta: =========";
        LOG(INFO) << table->fileMeta()->toString();
        EXPECT_EQ(SSTType::MAJOR, table->fileMeta()->sstType());
        EXPECT_EQ(SSTVersion::V1, table->fileMeta()->sstVersion());
        EXPECT_EQ(FilterPolicyType::BLOOM_FILTER, table->fileMeta()->filterPolicyType());
        EXPECT_EQ(10, table->fileMeta()->bitsPerKey());
        EXPECT_EQ(ROW_NUM * 2 * COL_NUM, table->fileMeta()->cellNum());
    }

public:
    constexpr static int ROW_NUM = 2048 * 2;
    constexpr static int ROWKEY_LEN = 16;
    constexpr static int COL_NUM = 8;
    constexpr static int COL_LEN = 8;
    constexpr static int VAL_LEN = 32;
    constexpr static const char* CF1 = "cf1";
    constexpr static const char* CF2 = "cf2";
    ReadOptionsRef read_options;
    Status st;
};

TEST_F(SSTableTest, init) {
    auto build_options = new_build_options();
    build_options->compression_type = CompressionType::NONE;
    build_options->sst_id = 1;
    build_sst(0, build_options);
    build_options->compression_type = CompressionType::SNAPPY;
    build_options->sst_id = 2;
    build_sst(1, build_options);
    build_options->compression_type = CompressionType::ZSTD;
    build_options->sst_id = 3;
    build_sst(2, build_options);
}

TEST_F(SSTableTest, table_without_compression) { seek_from_sst(0); }

TEST_F(SSTableTest, table_with_snappy_compression) { seek_from_sst(1); }

TEST_F(SSTableTest, table_with_zstd_compression) { seek_from_sst(2); }

TEST_F(SSTableTest, scan_all) {
    auto sst_file = sst_files[0];
    auto cells = cellses[0];
    LOG(INFO) << "cells: " << cells.size();

    auto table = pl::SSTable::open(read_options, sst_file, &st);
    EXPECT_TRUE(st.isOk());

    check_table(table.get());

    auto citer = cells.begin();
    auto iter = table->iterator();

    iter->first();
    while (iter->valid()) {
        EXPECT_TRUE(citer != cells.end());

        auto cell = iter->cell();
        auto cexp = *citer;

        EXPECT_EQ(cexp.rowkey, cell->rowkey());
        EXPECT_EQ(cexp.cf, cell->cf());
        EXPECT_EQ(cexp.col, cell->col());
        EXPECT_EQ(cexp.type, cell->cellType());
        EXPECT_EQ(cexp.ts, cell->timestamp());

        iter->next();
        ++citer;
    }
    EXPECT_TRUE(citer == cells.end());
}

TEST_F(SSTableTest, range_scan) {
    auto sst_file = sst_files[0];
    auto cells = cellses[0];

    auto table = pl::SSTable::open(read_options, sst_file, &st);
    EXPECT_TRUE(st.isOk());
    check_table(table.get());

    // 随机取一个范围的起始位置
    std::srand(std::time(nullptr));
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, ROW_NUM);

    auto iter = table->iterator();
    for (int i = 0; i < 3; ++i) {
        auto citer = cells.begin();
        int j = dis(gen) * 2 * COL_NUM;
        for (int m = 0; m < j; ++m) {
            ++citer;
        }
        std::string_view search_key = citer->rowkey;
        LOG(INFO) << "scan rowkey >= " << search_key;
        iter->seek(search_key);
        while (iter->valid()) {
            EXPECT_TRUE(citer != cells.end());
            auto cell = iter->cell();
            auto cexp = *citer;
            EXPECT_EQ(cexp.rowkey, cell->rowkey());
            EXPECT_EQ(cexp.cf, cell->cf());
            EXPECT_EQ(cexp.col, cell->col());
            EXPECT_EQ(cexp.type, cell->cellType());
            EXPECT_EQ(cexp.ts, cell->timestamp());

            iter->next();
            ++citer;
        }
    }
}

TEST_F(SSTableTest, query) {
    auto sst_file = sst_files[0];
    auto cells = cellses[0];
    LOG(INFO) << "cells: " << cells.size();

    auto table = pl::SSTable::open(read_options, sst_file, &st);
    EXPECT_TRUE(st.isOk());

    check_table(table.get());

    // 整行query
    std::srand(std::time(nullptr));
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, ROW_NUM);
    Arena buf;
    for (int i = 0; i < 333; ++i) {
        int idx = dis(gen) * 2 * COL_NUM;
        auto citer = cells.begin();
        for (int j = 0; j < idx; ++j) {
            ++citer;
        }

        std::string search_key = citer->rowkey;

        CellVecRef row;
        auto st = table->get(search_key, &buf, &row);
        EXPECT_TRUE(st.isOk());
        EXPECT_EQ(16, row.size());

        for (auto& cell : row) {
            auto cexp = *citer;
            EXPECT_EQ(cexp.rowkey, cell->rowkey());
            EXPECT_EQ(cexp.cf, cell->cf());
            EXPECT_EQ(cexp.col, cell->col());
            EXPECT_EQ(cexp.ts, cell->timestamp());
            EXPECT_EQ(cexp.type, cell->cellType());
            ++citer;
        }
    }

    // 空query
    for (int i = 0; i < 666; ++i) {
        auto rowkey = pl::random_string(ROWKEY_LEN * 3);
        CellVecRef cells;
        auto st = table->get(rowkey, &buf, &cells);
        EXPECT_TRUE(st.isNotFound());
        EXPECT_TRUE(cells.empty());
    }
}

TEST_F(SSTableTest, cleanup) {
    for (const auto& sst_file : sst_files) {
        std::remove(sst_file.c_str());
    }
}

} // namespace pl
