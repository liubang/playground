// Copyright (c) 2025 The Authors. All rights reserved.
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

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// BoundedTaskQueue — 有界任务队列, 支持多生产者单消费者
// ---------------------------------------------------------------------------
// 生产者 (brpc 线程): try_enqueue() — 非阻塞, 满时返回 false
// 消费者 (Service 线程): dequeue() — 阻塞等待
//
// 线程管理由调用方负责:
//   - 消费者在独立的 std::thread 中调用 dequeue() 循环
//   - shutdown() 唤醒消费者, dequeue 返回 std::nullopt
//
template <typename Task> class BoundedTaskQueue final : public DisableCopyAndMove {
public:
    explicit BoundedTaskQueue(size_t capacity) : capacity_(capacity) {}

    // 生产者: 尝试入队, 非阻塞. 队列满时返回 false.
    bool try_enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_.load(std::memory_order_relaxed)) {
                return false;
            }
            if (queue_.size() >= capacity_) {
                return false;
            }
            queue_.push(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    // 消费者: 阻塞等待直到有任务或 shutdown.
    // shutdown 后返回 std::nullopt.
    std::optional<Task> dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock,
                 [this] { return !queue_.empty() || shutdown_.load(std::memory_order_relaxed); });
        if (shutdown_.load(std::memory_order_relaxed) && queue_.empty()) {
            return std::nullopt;
        }
        Task task = std::move(queue_.front());
        queue_.pop();
        return task;
    }

    // 关闭队列, 唤醒消费者
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] bool is_shutdown() const { return shutdown_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> queue_;
    size_t capacity_;
    std::atomic<bool> shutdown_{false};
};

} // namespace pl::minitable::master
