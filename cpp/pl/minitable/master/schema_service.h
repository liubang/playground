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
// Created: 2026/06/23 23:40

#pragma once

#include <functional>
#include <thread>

#include "cpp/pl/minitable/master/bounded_task_queue.h"
#include "cpp/pl/utility/utility.h"

namespace pl::minitable::master {

// ---------------------------------------------------------------------------
// SchemaService — DDL 编排服务
// ---------------------------------------------------------------------------
// 拥有 1 个独立线程 + 有界任务队列 (容量 64).
// brpc 线程通过 try_enqueue() 投递 DDL 任务, 满时返回 false (拒绝请求).
//
// 工作流程:
//   1. brpc 线程 → try_enqueue(lambda)
//   2. worker 线程 → dequeue → 执行 lambda (schema 解析, 兼容检查, braft::apply)
//   3. braft on_apply 完成后回调 → done(Status)
//
class SchemaService final : public DisableCopyAndMove {
public:
    using Task = std::function<void()>;

    explicit SchemaService(size_t queue_capacity = 64)
        : queue_(queue_capacity), worker_(&SchemaService::worker_loop, this) {}

    ~SchemaService() { shutdown(); }

    // 生产者: brpc 线程调用, 非阻塞. 队列满时返回 false.
    [[nodiscard]] bool try_enqueue(Task task) { return queue_.try_enqueue(std::move(task)); }

    // 关闭服务, 等待 worker 线程退出
    void shutdown() {
        queue_.shutdown();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] size_t queue_size() const { return queue_.size(); }

private:
    void worker_loop() {
        while (true) {
            auto task = queue_.dequeue();
            if (!task.has_value()) {
                break; // shutdown
            }
            (*task)();
        }
    }

    BoundedTaskQueue<Task> queue_;
    std::thread worker_;
};

} // namespace pl::minitable::master
