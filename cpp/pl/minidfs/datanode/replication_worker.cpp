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
// Created: 2026/05/10 17:45

#include "cpp/pl/minidfs/datanode/replication_worker.h"

#include <folly/logging/xlog.h>

namespace pl::minidfs {

// ============================================================================
// Construction & Destruction
// ============================================================================

ReplicationWorker::ReplicationWorker(Config config, LocalBlockStore* store, CopyFunc copy_func)
    : config_(std::move(config)), store_(store), copy_func_(std::move(copy_func)) {}

ReplicationWorker::~ReplicationWorker() { stop(); }

// ============================================================================
// Lifecycle
// ============================================================================

void ReplicationWorker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    threads_.reserve(config_.max_concurrent_tasks);
    for (uint32_t i = 0; i < config_.max_concurrent_tasks; ++i) {
        threads_.emplace_back([this] {
            worker_loop();
        });
    }
}

void ReplicationWorker::stop() {
    running_.store(false, std::memory_order_relaxed);
    queue_cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

// ============================================================================
// Task Queue
// ============================================================================

void ReplicationWorker::enqueue(DataNodeTask task) {
    {
        std::lock_guard lock(queue_mu_);
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

size_t ReplicationWorker::pending_count() const {
    std::lock_guard lock(queue_mu_);
    return task_queue_.size();
}

// ============================================================================
// Worker Loop
// ============================================================================

void ReplicationWorker::worker_loop() {
    while (true) {
        DataNodeTask task;
        {
            std::unique_lock lock(queue_mu_);
            queue_cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_relaxed) || !task_queue_.empty();
            });

            if (!running_.load(std::memory_order_relaxed) && task_queue_.empty()) {
                return;
            }
            if (task_queue_.empty()) {
                continue;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        switch (task.kind) {
            case TaskKind::kCopy:
                execute_copy(task);
                break;
            case TaskKind::kDelete:
                execute_delete(task);
                break;
        }
    }
}

// ============================================================================
// Task Execution
// ============================================================================

void ReplicationWorker::execute_copy(const DataNodeTask& task) {
    // Read full block data from local store
    auto data_result = store_->read_block_data(task.block_id, task.generation_stamp);
    if (data_result.hasError()) {
        XLOGF(ERR, "replication copy failed: cannot read block {}: {}", task.block_id,
              data_result.error().describe());
        return;
    }

    // Send to target via the copy function (pipeline protocol)
    auto copy_result = copy_func_(task.block_id, task.generation_stamp, data_result.value(),
                                  task.target_host, task.target_port);
    if (copy_result.hasError()) {
        XLOGF(ERR, "replication copy failed: send to {}:{} for block {}: {}", task.target_host,
              task.target_port, task.block_id, copy_result.error().describe());
        return;
    }

    XLOGF(INFO, "replication copy succeeded: block {} -> {}:{}", task.block_id, task.target_host,
          task.target_port);
}

void ReplicationWorker::execute_delete(const DataNodeTask& task) {
    auto result = store_->delete_block(task.block_id, task.generation_stamp);
    if (result.hasError()) {
        XLOGF(WARN, "replication delete failed for block {}: {}", task.block_id,
              result.error().describe());
        return;
    }

    XLOGF(INFO, "replication delete succeeded: block {}", task.block_id);
}

} // namespace pl::minidfs
