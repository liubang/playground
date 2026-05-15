// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/05/15 23:41

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pl::flux::execution {

class TaskExecutor {
public:
    explicit TaskExecutor(size_t worker_count = 0);
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    template <typename F>
    [[nodiscard]] auto Submit(F&& task) -> std::future<std::invoke_result_t<F>> {
        using Result = std::invoke_result_t<F>;
        auto packaged = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(task));
        std::future<Result> future = packaged->get_future();
        Enqueue([packaged]() {
            (*packaged)();
        });
        return future;
    }

    void Shutdown();

private:
    void Enqueue(std::function<void()> task);
    void WorkerLoop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool shutdown_ = false;
};

} // namespace pl::flux::execution
