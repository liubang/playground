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

#include "cpp/misc/fs/fs.h"
#include "cpp/misc/sst/table.h"
#include "cpp/tools/scope.h"

#include <gtest/gtest.h>
#include <iostream>

TEST(table, table) {
    auto* options = new pl::Options();
    pl::FsReader* reader;
    auto* fs = pl::Fs::getInstance();
    fs->newFsReader("/tmp/test.sst", &reader);
    pl::Table* table;

    auto s = pl::Table::open(options, reader, reader->size(), &table);
    EXPECT_TRUE(s.isOk());
    std::cout << s.msg() << std::endl;

    SCOPE_EXIT {
        delete options->comparator;
        delete options->filter_policy;
        delete options;
        delete reader;
        delete table;
    };

    auto handle_result = [](void* arg, const pl::Binary& k, const pl::Binary& v) {
        std::string* saver = reinterpret_cast<std::string*>(arg);
        saver->assign(v.data(), v.size());
    };

    constexpr int COUNT = 10001;

    for (int i = 0; i < COUNT; ++i) {
        const std::string key_prefix = "test_key_";
        std::string key = key_prefix + std::to_string(i);
        std::string val;
        auto s = table->get(key, &val, handle_result);
        // TODO: the result is error
        if (s.isOk()) {
            ::printf("val is %s\n", val.c_str());
        } else {
            ::printf("key %s is not found\n", key.c_str());
        }
    }
}
