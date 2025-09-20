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

#include "cpp/pl/thread/thread_pool.h"
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <utility>
#include <vector>

TEST(thread_pool, thread_pool) {
    int n = 1024;
    pl::ThreadPool pool(n);
    std::vector<std::future<std::pair<int, int>>> results;
    auto start = std::chrono::system_clock::now();
    results.reserve(2 * n);
    for (int i = 0; i < 2 * n; ++i) {
        results.emplace_back(pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::make_pair(i, i);
        }));
    }

    for (auto& result : results) {
        std::pair<int, int> pair = result.get();
        EXPECT_EQ(pair.first, pair.second);
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - start);

    std::cout << "Elapsed time: " << duration.count() << "(ms)" << std::endl;
}
