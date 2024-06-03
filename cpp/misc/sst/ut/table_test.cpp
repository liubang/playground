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
#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/misc/sst/table.h"
#include "cpp/tools/random.h"

#include <gtest/gtest.h>

namespace pl {
class SSTableTest : public ::testing::Test {
    void SetUp() override {
        options = std::make_shared<Options>();
        options->compression_type = CompressionType::kSnappyCompression;
        fs = std::make_shared<PosixFs>();

        build_sst();
    }

    void TearDown() override { ::printf("teardown!!!!\n"); }

public:
    void build_sst() {
        ::printf("build sst\n");
        auto writer = fs->newFsWriter(sst_file, &st);
        EXPECT_TRUE(st.isOk());
        auto sstable_builder = std::make_unique<pl::SSTableBuilder>(options, std::move(writer));
        std::vector<std::string> keys;
        const std::string key_prefix = "test_key_";
        for (int i = 0; i < KEY_COUNT; ++i) {
            std::string key = key_prefix + std::to_string(i);
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        for (int i = 0; i < KEY_COUNT; ++i) {
            auto val = pl::random_string(64);
            sstable_builder->add(keys[i], val);
            kvs[keys[i]] = val;
        }
        sstable_builder->finish();
    }

public:
    constexpr static int KEY_COUNT = 10001;
    const std::string sst_file = "/tmp/test.sst";
    std::unordered_map<std::string, std::string> kvs;
    OptionsRef options;
    FsRef fs;
    Status st;
};

TEST_F(SSTableTest, table) {
    auto reader = fs->newFsReader("/tmp/test.sst", &st);
    EXPECT_TRUE(st.isOk());
    std::size_t sst_size = reader->size();
    ::printf("file: %s, size: %zu\n", "/tmp/test.sst", sst_size);
    auto table = pl::Table::open(options, std::move(reader), sst_size, &st);
    EXPECT_TRUE(st.isOk());

    auto handle_result = [](void* arg, const pl::Binary& k, const pl::Binary& v) {
        auto* saver = reinterpret_cast<std::string*>(arg);
        saver->assign(v.data(), v.size());
    };

    for (int i = 0; i < KEY_COUNT; ++i) {
        const std::string key_prefix = "test_key_";
        std::string key = key_prefix + std::to_string(i);
        std::string val;
        st = table->get(key, &val, handle_result);
        EXPECT_TRUE(st.isOk());
        EXPECT_EQ(kvs[key], val);
    }

    for (int i = 0; i < KEY_COUNT; ++i) {
        const std::string key_prefix = "key_not_exist_";
        std::string key = key_prefix + std::to_string(i);
        std::string val;
        st = table->get("not exist", &val, handle_result);
        EXPECT_TRUE(st.isNotFound());
    }
}

} // namespace pl
