// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/05/21 23:50

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <utility>
#include <vector>

#include "cpp/pl/thread/thread_pool.h"

// =========================================================================
// 无界 — 原有行为保持不变
// =========================================================================

TEST(ThreadPool, UnboundedSubmit) {
    int n = 32;
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

// =========================================================================
// 有界 try_submit — 满时返回 nullopt
// =========================================================================

TEST(ThreadPool, BoundedTrySubmit) {
    pl::ThreadPool pool(1, 1); // 1 thread, capacity 1
    std::atomic<int> count{0};

    // 提交 N 个任务, 至少一些成功
    std::vector<std::future<void>> futures;
    int ok = 0, fail = 0;
    for (int i = 0; i < 10; ++i) {
        auto f = pool.try_submit([&] { count.fetch_add(1, std::memory_order_relaxed); });
        if (f.has_value()) {
            futures.push_back(std::move(*f));
            ++ok;
        } else {
            ++fail;
        }
    }
    EXPECT_GE(ok, 1) << "at least some tasks should succeed";
    EXPECT_GE(fail, 1) << "capacity=1 should reject some tasks";

    for (auto& f : futures)
        f.get();
    EXPECT_EQ(count.load(), ok);
}

// =========================================================================
// try_submit 满-空-满 转换
// =========================================================================

TEST(ThreadPool, BoundedFillDrainFill) {
    pl::ThreadPool pool(1, 4);
    std::atomic<int> count{0};
    int expected = 0;

    for (int round = 0; round < 5; ++round) {
        std::vector<std::future<void>> futures;
        int ok = 0, fail = 0;
        for (int i = 0; i < 20; ++i) {
            auto f = pool.try_submit([&] {
                count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
            if (f.has_value()) {
                futures.push_back(std::move(*f));
                ++ok;
                ++expected;
            } else {
                ++fail;
            }
        }
        EXPECT_GE(ok, 1) << "round=" << round;
        EXPECT_GE(fail, 1) << "round=" << round;

        for (auto& f : futures)
            f.get();
    }
    EXPECT_EQ(count.load(), expected);
}

// =========================================================================
// submit 阻塞等待 (有界队列满时)
// =========================================================================

TEST(ThreadPool, BoundedSubmitBlocks) {
    pl::ThreadPool pool(1, 1); // 1 worker, capacity 1
    std::atomic<bool> block{true};

    // 提交 1 个阻塞任务, worker 开始执行
    auto f1 = pool.submit([&] {
        while (block.load())
            std::this_thread::yield();
    });

    // 提交第 2 个: 填满队列 (size 1 == capacity)
    auto f2 = pool.submit([&] {
        while (block.load())
            std::this_thread::yield();
    });

    // 此时队列满, submit 应该阻塞 — 在单独线程中验证
    std::atomic<bool> submit_done{false};
    int result = 0;

    std::thread submitter([&] {
        auto f3 = pool.submit([] { return 42; });
        result = f3.get();
        submit_done.store(true);
    });

    // 等 submitter 排队
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(submit_done.load());

    // 释放 → 队列排空 → submit 解除阻塞
    block.store(false);
    f1.get();
    f2.get();

    submitter.join();
    EXPECT_TRUE(submit_done.load());
    EXPECT_EQ(result, 42);
}

// =========================================================================
// pending 和 capacity
// =========================================================================

TEST(ThreadPool, PendingAndCapacity) {
    pl::ThreadPool pool(1, 10);
    EXPECT_EQ(pool.capacity(), 10);
    EXPECT_EQ(pool.pending(), 0);

    std::atomic<bool> block{true};
    for (int i = 0; i < 5; ++i) {
        pool.submit([&] {
            while (block.load())
                std::this_thread::yield();
        });
    }
    // Worker 在执行第 1 个(阻塞), 剩余 4 个在队列
    EXPECT_GT(pool.pending(), 0);
    EXPECT_LE(pool.pending(), 5);

    block.store(false);
    // 等待排空 — 提交一个快速任务并等待完成
    pool.submit([] {}).get();
    EXPECT_EQ(pool.pending(), 0);
}

// =========================================================================
// 单线程串行执行验证
// =========================================================================

TEST(ThreadPool, SerialExecution) {
    pl::ThreadPool pool(1, 16);
    std::vector<int> order;
    std::mutex mtx;

    for (int i = 0; i < 10; ++i) {
        pool.submit([i, &order, &mtx] {
            std::lock_guard<std::mutex> lock(mtx);
            order.push_back(i);
        });
    }
    // 等待所有任务完成 — 最后一个 future get
    auto f = pool.submit([] { return 0; });
    f.get();

    ASSERT_EQ(order.size(), 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(order[i], i); // 串行执行保证顺序
    }
}

// =========================================================================
// void 返回值
// =========================================================================

TEST(ThreadPool, VoidReturn) {
    pl::ThreadPool pool(2, 4);
    auto f = pool.try_submit([] {});
    ASSERT_TRUE(f.has_value());
    f->get(); // void future
}
