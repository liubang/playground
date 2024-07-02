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

#include "cpp/pl/fs/posix_fs.h"
#include "cpp/pl/log/logger.h"
#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/sstable.h"
#include "cpp/pl/sst/sstable_builder.h"

#include <gtest/gtest.h>
#include <set>

namespace pl {
class SSTableTest : public ::testing::Test {
    void SetUp() override {
        build_options = std::make_shared<BuildOptions>();
        build_options->compression_type = CompressionType::NONE;
        build_options->sst_type = SSTType::MAJOR;
        build_options->sst_version = SSTVersion::V1;
        build_options->filter_type = FilterPolicyType::BLOOM_FILTER;

        read_options = std::make_shared<ReadOptions>();
        fs = std::make_shared<PosixFs>();
    }

    void TearDown() override { cells.clear(); }

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

public:
    void build_sst() {
        auto writer = fs->newFsWriter(sst_file, &st);
        EXPECT_TRUE(st.isOk());
        auto sstable_builder =
            std::make_unique<pl::SSTableBuilder>(build_options, std::move(writer));
        for (int i = 0; i < ROW_COUNT; ++i) {
            std::string rowkey = pl::random_string(KEY_LEN + (i % KEY_LEN)) + std::to_string(i);
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

    void seek_from_sst() {
        auto reader = fs->newFsReader(this->sst_file, &st);
        EXPECT_TRUE(st.isOk());
        std::size_t sst_size = reader->size();
        LOG_INFO << "file: " << sst_file << ", size: " << sst_size;
        auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
        if (!st.isOk()) {
            LOG_ERROR << "open table failed, error: " << st.msg();
        }
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

    // void scan_from_sst() {
    //     auto reader = fs->newFsReader(this->sst_file, &st);
    //     EXPECT_TRUE(st.isOk());
    //     std::size_t sst_size = reader->size();
    //     ::printf("file: %s, size: %zu\n", this->sst_file.c_str(), sst_size);
    //     auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
    //     EXPECT_TRUE(st.isOk());
    //
    //     check_table(table.get());
    //
    //     auto iter = table->iterator();
    //     int idx = 0;
    //     auto key_iter = keys.begin();
    //     iter->first();
    //     while (iter->valid()) {
    //         auto key = iter->key();
    //         auto val = iter->val();
    //         EXPECT_EQ(*key_iter, key);
    //         EXPECT_EQ(kvs[std::string(key)], val);
    //         idx++;
    //         key_iter++;
    //         iter->next();
    //     }
    //     EXPECT_EQ(ROW_COUNT, idx);
    // }
    //
    // void range_scan_from_sst() {
    //     auto reader = fs->newFsReader(this->sst_file, &st);
    //     EXPECT_TRUE(st.isOk());
    //     std::size_t sst_size = reader->size();
    //     ::printf("file: %s, size: %zu\n", this->sst_file.c_str(), sst_size);
    //     auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
    //     EXPECT_TRUE(st.isOk());
    //     check_table(table.get());
    //     auto iter = table->iterator();
    //     int idx = 0;
    //     std::string search_key = "f";
    //     ::printf("WHERE KEY >= %s\n", search_key.c_str());
    //     iter->seek(search_key);
    //     while (iter->valid()) {
    //         auto key = iter->key();
    //         EXPECT_TRUE(key.compare(search_key) >= 0);
    //         idx++;
    //         iter->next();
    //     }
    //     ::printf("row_count: %d\n", idx);
    // }

private:
    void check_table(SSTable* table) {
        LOG_INFO << "======== sst file meta: =========";
        LOG_INFO << table->fileMeta()->toString();
        EXPECT_EQ(SSTType::MAJOR, table->fileMeta()->sstType());
        EXPECT_EQ(SSTVersion::V1, table->fileMeta()->sstVersion());
        EXPECT_EQ(FilterPolicyType::BLOOM_FILTER, table->fileMeta()->filterPolicyType());
        EXPECT_EQ(10, table->fileMeta()->bitsPerKey());
    }

public:
    constexpr static int ROW_COUNT = 2 * 2;
    constexpr static int KEY_LEN = 16;
    constexpr static int COL_NUM = 8;
    constexpr static int COL_LEN = 8;
    constexpr static int VAL_LEN = 32;
    constexpr static const char* CF1 = "cf1";
    constexpr static const char* CF2 = "cf2";
    std::string sst_file;
    std::set<CaseCell, CaseCellComparator> cells;
    BuildOptionsRef build_options;
    ReadOptionsRef read_options;
    FsRef fs;
    Status st;
};

TEST_F(SSTableTest, table_without_compression) {
    build_options->compression_type = CompressionType::NONE;
    build_options->sst_id = 1;
    build_options->patch_id = 1;
    this->sst_file = "/tmp/1.sst";
    this->build_sst();
    this->seek_from_sst();
    // this->scan_from_sst();
    // this->range_scan_from_sst();
}

// TEST_F(SSTableTest, table_with_snappy_compression) {
//     build_options->compression_type = CompressionType::SNAPPY;
//     build_options->sst_id = 2;
//     build_options->patch_id = 2;
//     this->sst_file = "/tmp/2.sst";
//     this->build_sst();
//     this->seek_from_sst();
//     this->scan_from_sst();
//     this->range_scan_from_sst();
// }
//
// TEST_F(SSTableTest, table_with_zstd_compression) {
//     build_options->compression_type = CompressionType::ZSTD;
//     build_options->sst_id = 3;
//     build_options->patch_id = 3;
//     this->sst_file = "/tmp/3.sst";
//     this->build_sst();
//     this->seek_from_sst();
//     this->scan_from_sst();
//     this->range_scan_from_sst();
// }

} // namespace pl
