//=====================================================================
//
// thread_pool_test.cpp -
//
// Created by liubang on 2023/05/21 23:37
// Last Modified: 2023/05/21 23:37
//
//=====================================================================

#include "cpp/tools/thread_pool.h"

#include <chrono>
#include <future>
#include <iostream>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

TEST(thread_pool, thread_pool) {
    pl::ThreadPool pool(1024);
    std::vector<std::future<std::pair<int, int>>> results;
    auto start = std::chrono::system_clock::now();
    results.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
        results.emplace_back(pool.enqueue([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return std::make_pair(i, i);
        }));
    }

    for (auto &result : results) {
        std::pair<int, int> pair = result.get();
        EXPECT_EQ(pair.first, pair.second);
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - start);

    std::cout << duration.count() << std::endl;
}
