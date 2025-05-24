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

    // 添加相等比较操作符，便于测试验证
    bool operator==(const CaseCell& other) const {
        return rowkey == other.rowkey && cf == other.cf && col == other.col && type == other.type &&
               ts == other.ts && val == other.val;
    }
};

struct CaseCellComparator {
    bool operator()(const CaseCell& lhs, const CaseCell& rhs) const {
        // 使用 tie 简化比较逻辑
        return std::tie(lhs.rowkey, lhs.cf, lhs.col, rhs.ts, lhs.type) <
               std::tie(rhs.rowkey, rhs.cf, rhs.col, lhs.ts, rhs.type);
    }
};

namespace {

// 测试配置常量
struct TestConfig {
    static constexpr int ROW_NUM = 2048 * 2;
    static constexpr int ROWKEY_LEN = 16;
    static constexpr int COL_NUM = 8;
    static constexpr int COL_LEN = 8;
    static constexpr int VAL_LEN = 32;
    static constexpr int QUERY_TEST_COUNT = 333;
    static constexpr int EMPTY_QUERY_TEST_COUNT = 666;
    static constexpr int RANGE_SCAN_TEST_COUNT = 3;
    static constexpr const char* CF1 = "cf1";
    static constexpr const char* CF2 = "cf2";
    static constexpr const char* TEST_DIR = "/tmp/MAJOR";
};

class TestDataManager {
public:
    static TestDataManager& instance() {
        static TestDataManager instance;
        return instance;
    }

    [[nodiscard]] const std::vector<std::string>& sst_files() const { return sst_files_; }
    [[nodiscard]] const std::vector<std::set<CaseCell, CaseCellComparator>>& cell_sets() const {
        return cellses_;
    }
    std::set<CaseCell, CaseCellComparator>& cell_set(size_t idx) { return cellses_[idx]; }

private:
    TestDataManager()
        : sst_files_{"/tmp/MAJOR/1.sst", "/tmp/MAJOR/2.sst", "/tmp/MAJOR/3.sst"}, cellses_(3) {}

    std::vector<std::string> sst_files_;
    std::vector<std::set<CaseCell, CaseCellComparator>> cellses_;
};

class RandomGenerator {
public:
    static RandomGenerator& instance() {
        static RandomGenerator instance;
        return instance;
    }

    int random_int(int min, int max) {
        std::uniform_int_distribution<> dis(min, max);
        return dis(gen_);
    }

private:
    RandomGenerator() : gen_(rd_()) {}

    std::random_device rd_;
    std::mt19937 gen_;
};

} // namespace

class SSTableTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        create_test_directory();
        build_all_sstables();
    }

    static void TearDownTestSuite() { cleanup_test_files(); }

private:
    static void create_test_directory() {
        std::filesystem::create_directories(TestConfig::TEST_DIR);
    }

    static void cleanup_test_files() {
        const auto& sst_files = TestDataManager::instance().sst_files();
        for (const auto& file : sst_files) {
            std::filesystem::remove(file);
        }
        std::filesystem::remove_all(TestConfig::TEST_DIR);
    }

    static BuildOptionsRef new_build_options(
        CompressionType compression = CompressionType::NONE,
        FilterPolicyType filter = FilterPolicyType::STANDARD_BLOOM_FILTER,
        int sst_id = 1) {
        auto build_options = std::make_shared<BuildOptions>();
        build_options->data_dir = "/tmp";
        build_options->compression_type = compression;
        build_options->sst_type = SSTType::MAJOR;
        build_options->sst_version = SSTVersion::V1;
        build_options->filter_type = filter;
        build_options->sst_id = sst_id;
        return build_options;
    }

    static void build_all_sstables() {
        // 构建三个不同配置的SSTable
        const std::vector<std::tuple<CompressionType, FilterPolicyType, int>> configs = {
            {CompressionType::NONE, FilterPolicyType::STANDARD_BLOOM_FILTER, 1},
            {CompressionType::SNAPPY, FilterPolicyType::STANDARD_BLOOM_FILTER, 2},
            {CompressionType::ZSTD, FilterPolicyType::BLOCKED_BLOOM_FILTER, 3}};

        for (size_t i = 0; i < configs.size(); ++i) {
            auto [compression, filter, sst_id] = configs[i];
            auto build_options = new_build_options(compression, filter, sst_id);
            build_sst(i, build_options);
        }
    }

    static void build_sst(int idx, const BuildOptionsRef& build_options) {
        auto& cells = TestDataManager::instance().cell_set(idx);
        auto sstable_builder = std::make_unique<pl::SSTableBuilder>(build_options);
        auto result = sstable_builder->open();
        ASSERT_TRUE(result.hasValue())
            << "Failed to open SSTable builder: " << result.error().describe();

        generate_test_data(cells);

        for (const auto& cell : cells) {
            sstable_builder->add(cell.to_cell());
        }
        result = sstable_builder->finish();
        ASSERT_TRUE(result.hasValue())
            << "Failed to finish SSTable building: " << result.error().describe();
    }

    static void generate_test_data(std::set<CaseCell, CaseCellComparator>& cells) {
        const uint64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
        for (int i = 0; i < TestConfig::ROW_NUM; ++i) {
            std::string rowkey =
                pl::random_string(TestConfig::ROWKEY_LEN + (i % TestConfig::ROWKEY_LEN)) +
                std::to_string(i);

            for (int j = 0; j < TestConfig::COL_NUM; ++j) {
                // 生成两个column family的数据
                for (const char* cf : {TestConfig::CF1, TestConfig::CF2}) {
                    CaseCell cell{.rowkey = rowkey,
                                  .cf = cf,
                                  .col = pl::random_string(TestConfig::COL_LEN),
                                  .type = static_cast<CellType>(i % 3),
                                  .ts = current_time + i, // 确保时间戳的唯一性
                                  .val = pl::random_string(TestConfig::VAL_LEN)};
                    cells.insert(cell);
                }
            }
        }
    }

protected:
    // 辅助方法：打开SSTable
    static std::unique_ptr<SSTable> open_sstable(size_t idx,
                                                 const ReadOptionsRef& read_options = nullptr) {
        auto options = read_options ? read_options : std::make_shared<ReadOptions>();
        const auto& sst_file = TestDataManager::instance().sst_files()[idx];

        auto result = pl::SSTable::open(options, sst_file);
        EXPECT_TRUE(result.hasValue()) << "Failed to open SSTable: " << sst_file;
        return result.hasValue() ? std::move(result.value()) : nullptr;
    }

    // 辅助方法：验证SSTable基本信息
    static void check_table(SSTable* table) {
        XLOGF(INFO, "======= SST file meta: =======");
        XLOGF(INFO, "{}", table->fileMeta()->toString());

        EXPECT_EQ(SSTType::MAJOR, table->fileMeta()->sstType());
        EXPECT_EQ(SSTVersion::V1, table->fileMeta()->sstVersion());
        EXPECT_EQ(10, table->fileMeta()->bitsPerKey());
        EXPECT_EQ(TestConfig::ROW_NUM * 2 * TestConfig::COL_NUM, table->fileMeta()->cellNum());
    }

    // 辅助方法：验证cell内容
    static void verify_cell_content(const CaseCell& expected, const CellRef& actual) {
        EXPECT_EQ(expected.rowkey, actual->rowkey());
        EXPECT_EQ(expected.cf, actual->cf());
        EXPECT_EQ(expected.col, actual->col());
        EXPECT_EQ(expected.type, actual->cell_type());
        EXPECT_EQ(expected.ts, actual->timestamp());
    }

    static void seek_from_sst(int idx, const ReadOptionsRef& read_options = nullptr) {
        const auto& cells = TestDataManager::instance().cell_sets()[idx];
        auto table = open_sstable(idx, read_options);
        ASSERT_NE(table, nullptr);

        check_table(table.get());
        auto it = table->iterator();
        for (const auto& cell : cells) {
            it->seek(cell.rowkey);
            EXPECT_TRUE(it->valid()) << "Iterator invalid for rowkey: " << cell.rowkey;
            auto actual_cell = it->cell();
            ASSERT_NE(actual_cell, nullptr);
            EXPECT_EQ(cell.rowkey, actual_cell->rowkey());
        }
    }
};

TEST_F(SSTableTest, table_without_compression) { seek_from_sst(0); }

TEST_F(SSTableTest, table_with_snappy_compression) { seek_from_sst(1); }

TEST_F(SSTableTest, table_with_zstd_compression) { seek_from_sst(2); }

TEST_F(SSTableTest, scan_all) {
    const auto& cells = TestDataManager::instance().cell_sets()[0];
    auto table = open_sstable(0);
    ASSERT_NE(table, nullptr);
    check_table(table.get());
    XLOGF(INFO, "cells: {}", cells.size());

    auto cell_iter = cells.begin();
    auto table_iter = table->iterator();

    table_iter->first();
    while (table_iter->valid()) {
        EXPECT_TRUE(cell_iter != cells.end());
        auto actual_cell = table_iter->cell();
        ASSERT_NE(actual_cell, nullptr);
        verify_cell_content(*cell_iter, actual_cell);
        table_iter->next();
        ++cell_iter;
    }
    EXPECT_TRUE(cell_iter == cells.end());
}

TEST_F(SSTableTest, range_scan) {
    const auto& cells = TestDataManager::instance().cell_sets()[0];
    auto table = open_sstable(0);
    ASSERT_NE(table, nullptr);
    check_table(table.get());
    XLOGF(INFO, "cells: {}", cells.size());

    auto& random_gen = RandomGenerator::instance();

    auto table_iter = table->iterator();
    for (int i = 0; i < TestConfig::RANGE_SCAN_TEST_COUNT; ++i) {
        auto cell_iter = cells.begin();

        int advance_count = random_gen.random_int(0, TestConfig::ROW_NUM) * 2 * TestConfig::COL_NUM;
        std::advance(cell_iter, std::min(advance_count, static_cast<int>(cells.size()) - 1));

        std::string_view search_key = cell_iter->rowkey;
        XLOGF(INFO, "scan rowkey >= {}", search_key);
        table_iter->seek(search_key);
        while (table_iter->valid()) {
            EXPECT_TRUE(cell_iter != cells.end());
            auto actual_cell = table_iter->cell();
            verify_cell_content(*cell_iter, actual_cell);

            table_iter->next();
            ++cell_iter;
        }
    }
}

TEST_F(SSTableTest, query) {
    const auto& cells = TestDataManager::instance().cell_sets()[0];
    auto table = open_sstable(0);
    ASSERT_NE(table, nullptr);
    check_table(table.get());
    XLOGF(INFO, "cells: {}", cells.size());

    // 整行query
    auto& random_gen = RandomGenerator::instance();
    Arena buf;
    for (int i = 0; i < TestConfig::QUERY_TEST_COUNT; ++i) {
        auto cell_iter = cells.begin();
        int advance_count = random_gen.random_int(0, TestConfig::ROW_NUM) * 2 * TestConfig::COL_NUM;
        std::advance(cell_iter, std::min(advance_count, static_cast<int>(cells.size()) - 1));

        std::string search_key = cell_iter->rowkey;
        XLOGF(INFO, "search_key: {}", search_key);
        auto query_result = table->get(search_key, &buf);
        EXPECT_TRUE(query_result.hasValue());
        const CellVecRef& row_cells = query_result.value();
        EXPECT_EQ(16, row_cells.size());

        for (const auto& actual_cell : row_cells) {
            verify_cell_content(*cell_iter, actual_cell);
            ++cell_iter;
        }
    }

    // 空query
    for (int i = 0; i < TestConfig::EMPTY_QUERY_TEST_COUNT; ++i) {
        std::string non_existent_rowkey = pl::random_string(TestConfig::ROWKEY_LEN * 3);
        auto query_result = table->get(non_existent_rowkey, &buf);
        EXPECT_TRUE(query_result.hasError())
            << "Query should fail for non-existent rowkey: " << non_existent_rowkey;
        if (query_result.hasError()) {
            EXPECT_EQ(StatusCode::kKVStoreNotFound, query_result.error().code());
        }
    }
}

} // namespace pl
