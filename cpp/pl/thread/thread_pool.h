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

#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pl {

class ThreadPool {
    using Task = std::function<void()>;

public:
    ThreadPool(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back(&ThreadPool::execute, this);
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F(Args...)>> {
        using RetrunType = std::invoke_result_t<F(Args...)>;
        auto task = std::packaged_task<RetrunType>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        auto future = task.get_future();
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("");
            }
            tasks_.emplace([task = std::move(task)]() {
                task();
            });
        }
        condition_.notify_one();
        return future;
    }

private:
    void execute() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(queue_mutex_);
                if (stop_ && tasks_.empty()) {
                    break;
                }
                condition_.wait(lk, [this] {
                    return this->stop_ || !this->tasks_.empty();
                });
                task = this->tasks_.front();
                this->tasks_.pop();
            }
            task();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};

} // namespace pl
