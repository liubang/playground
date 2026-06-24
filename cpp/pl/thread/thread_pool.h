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

#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pl {

class ThreadPool {
    using Task = std::function<void()>;

public:
    // capacity: 0 = 无界 (默认), >0 = 有界上限
    explicit ThreadPool(size_t threads, size_t capacity = 0) : capacity_(capacity) {
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
        condition_full_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    // 无界提交: 队列满时阻塞等待 (capacity==0 时永不阻塞)
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F&&, Args&&...>> {
        using R = std::invoke_result_t<F&&, Args&&...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool: submit after stop");
            }
            if (capacity_ > 0) {
                condition_full_.wait(lk, [this] { return stop_ || tasks_.size() < capacity_; });
                if (stop_) {
                    throw std::runtime_error("ThreadPool: submit after stop");
                }
            }
            tasks_.emplace([task = std::move(task)]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    // 有界提交: 满时立即返回 std::nullopt (非阻塞)
    template <class F, class... Args>
    auto try_submit(F&& f, Args&&... args)
        -> std::optional<std::future<std::invoke_result_t<F&&, Args&&...>>> {
        using R = std::invoke_result_t<F&&, Args&&...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            if (stop_) {
                return std::nullopt;
            }
            if (capacity_ > 0 && tasks_.size() >= capacity_) {
                return std::nullopt;
            }
            tasks_.emplace([task = std::move(task)]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    // 队列中待处理任务数
    size_t pending() const {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    size_t capacity() const { return capacity_; }

private:
    void execute() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(queue_mutex_);
                condition_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // 唤醒阻塞中的 submit 生产者
            if (capacity_ > 0) {
                condition_full_.notify_one();
            }
            task();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;      // 消费者等待
    std::condition_variable condition_full_; // 生产者等待 (有界)
    size_t capacity_{0};
    bool stop_{false};
};

} // namespace pl
