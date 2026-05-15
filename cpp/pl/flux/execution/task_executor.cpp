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

#include "cpp/pl/flux/execution/task_executor.h"

#include <algorithm>
#include <stdexcept>

namespace pl::flux::execution {

TaskExecutor::TaskExecutor(size_t worker_count) {
    const size_t hardware = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t count = worker_count == 0 ? hardware : worker_count;
    workers_.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        workers_.emplace_back([this]() {
            WorkerLoop();
        });
    }
}

TaskExecutor::~TaskExecutor() { Shutdown(); }

void TaskExecutor::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void TaskExecutor::Enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) {
            throw std::runtime_error("task executor is shut down");
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void TaskExecutor::WorkerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this]() {
                return shutdown_ || !tasks_.empty();
            });
            if (shutdown_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

} // namespace pl::flux::execution
