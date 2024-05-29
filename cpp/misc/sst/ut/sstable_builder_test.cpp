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
#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/tools/random.h"
#include "cpp/tools/scope.h"

#include <gtest/gtest.h>
#include <vector>

TEST(sstable_builder, build) {
    constexpr int COUNT = 10001;
    auto* options = new pl::Options();
    pl::FsWriter* writer;
    auto* fs = pl::Fs::getInstance();
    fs->newFsWriter("/tmp/test.sst", &writer);

    auto* sstable_builder = new pl::SSTableBuilder(options, writer);

    SCOPE_EXIT {
        delete options->comparator;
        delete options->filter_policy;
        delete options;
        delete writer;
        delete sstable_builder;
    };

    std::vector<std::string> keys;
    std::unordered_map<std::string, std::string> kvs;
    const std::string key_prefix = "test_key_";
    for (int i = 0; i < COUNT; ++i) {
        std::string key = key_prefix + std::to_string(i);
        keys.push_back(key);
    }

    std::sort(keys.begin(), keys.end());

    for (int i = 0; i < COUNT; ++i) {
        auto val = pl::random_string(64);
        sstable_builder->add(keys[i], val);
        kvs[keys[i]] = val;
    }
    sstable_builder->finish();
}
