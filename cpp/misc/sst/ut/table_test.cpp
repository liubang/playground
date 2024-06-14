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

#include "cpp/misc/fs/posix_fs.h"
#include "cpp/misc/sst/sstable.h"
#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/tools/random.h"

#include <gtest/gtest.h>
#include <set>
#include <unordered_map>

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
    void TearDown() override {
        kvs.clear();
        keys.clear();
    }

public:
    void build_sst() {
        auto writer = fs->newFsWriter(sst_file, &st);
        EXPECT_TRUE(st.isOk());
        auto sstable_builder =
            std::make_unique<pl::SSTableBuilder>(build_options, std::move(writer));
        for (int i = 0; i < ROW_COUNT; ++i) {
            std::string key = pl::random_string(KEY_LEN) + std::to_string(i);
            keys.insert(key);
        }
        for (const auto& key : keys) {
            auto val = pl::random_string(VAL_LEN);
            sstable_builder->add(key, val);
            kvs[key] = val;
        }
        auto status = sstable_builder->finish();
        EXPECT_TRUE(status.isOk());
    }

    void seek_from_sst() {
        auto reader = fs->newFsReader(this->sst_file, &st);
        EXPECT_TRUE(st.isOk());
        std::size_t sst_size = reader->size();
        ::printf("file: %s, size: %zu\n", this->sst_file.c_str(), sst_size);
        auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
        if (!st.isOk()) {
            ::printf("open table failed, error: %s\n", st.msg().c_str());
        }
        EXPECT_TRUE(st.isOk());

        check_table(table.get());

        auto handle_result = [](void* arg, const pl::Binary& k, const pl::Binary& v) {
            auto* saver = reinterpret_cast<std::string*>(arg);
            saver->assign(v.data(), v.size());
        };

        for (const auto& key : keys) {
            std::string val;
            st = table->get(key, &val, handle_result);
            EXPECT_TRUE(st.isOk());
            EXPECT_EQ(kvs[key], val);
        }

        for (int i = 0; i < ROW_COUNT; ++i) {
            std::string key = pl::random_string(KEY_LEN + 2);
            std::string val;
            st = table->get(key, &val, handle_result);
            if (st.isOk()) {
                ::printf("key: %s, val: %s\n", key.c_str(), val.c_str());
            }
            EXPECT_TRUE(st.isNotFound());
        }
    }

    void scan_from_sst() {
        auto reader = fs->newFsReader(this->sst_file, &st);
        EXPECT_TRUE(st.isOk());
        std::size_t sst_size = reader->size();
        ::printf("file: %s, size: %zu\n", this->sst_file.c_str(), sst_size);
        auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
        EXPECT_TRUE(st.isOk());

        check_table(table.get());

        auto iter = table->iterator();
        int idx = 0;
        auto key_iter = keys.begin();
        iter->first();
        while (iter->valid()) {
            auto key = iter->key();
            auto val = iter->val();
            EXPECT_EQ(*key_iter, key.toString());
            EXPECT_EQ(kvs[key.toString()], val.toString());
            idx++;
            key_iter++;
            iter->next();
        }
        EXPECT_EQ(ROW_COUNT, idx);
    }

    void range_scan_from_sst() {
        auto reader = fs->newFsReader(this->sst_file, &st);
        EXPECT_TRUE(st.isOk());
        std::size_t sst_size = reader->size();
        ::printf("file: %s, size: %zu\n", this->sst_file.c_str(), sst_size);
        auto table = pl::SSTable::open(read_options, std::move(reader), sst_size, &st);
        EXPECT_TRUE(st.isOk());
        check_table(table.get());
        auto iter = table->iterator();
        int idx = 0;
        std::string search_key = "f";
        ::printf("WHERE KEY >= %s\n", search_key.c_str());
        iter->seek(search_key);
        while (iter->valid()) {
            auto key = iter->key();
            EXPECT_TRUE(key.compare(search_key) >= 0);
            idx++;
            iter->next();
        }
        ::printf("row_count: %d\n", idx);
    }

private:
    void check_table(SSTable* table) {
        ::printf("sst file meta:\n%s\n", table->fileMeta()->toString().c_str());
        EXPECT_EQ(SSTType::MAJOR, table->fileMeta()->sstType());
        EXPECT_EQ(SSTVersion::V1, table->fileMeta()->sstVersion());
        EXPECT_EQ(FilterPolicyType::BLOOM_FILTER, table->fileMeta()->filterPolicyType());
        EXPECT_EQ(10, table->fileMeta()->bitsPerKey());
    }

public:
    constexpr static int ROW_COUNT = 40960 * 2;
    constexpr static int KEY_LEN = 16;
    constexpr static int VAL_LEN = 32;
    std::string sst_file;
    std::unordered_map<std::string, std::string> kvs;
    std::set<std::string> keys;
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
    this->scan_from_sst();
    this->range_scan_from_sst();
}

TEST_F(SSTableTest, table_with_snappy_compression) {
    build_options->compression_type = CompressionType::SNAPPY;
    build_options->sst_id = 2;
    build_options->patch_id = 2;
    this->sst_file = "/tmp/2.sst";
    this->build_sst();
    this->seek_from_sst();
    this->scan_from_sst();
    this->range_scan_from_sst();
}

TEST_F(SSTableTest, table_with_zstd_compression) {
    build_options->compression_type = CompressionType::ZSTD;
    build_options->sst_id = 3;
    build_options->patch_id = 3;
    this->sst_file = "/tmp/3.sst";
    this->build_sst();
    this->seek_from_sst();
    this->scan_from_sst();
    this->range_scan_from_sst();
}

} // namespace pl
