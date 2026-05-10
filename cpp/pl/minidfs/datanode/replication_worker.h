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

#pragma once

#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/status/result.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace pl::minidfs {

// ============================================================================
// ReplicationTask — a unit of work for the replication worker.
//
// Two kinds:
//   - Copy: read a block from local store, send to a remote DN (pipeline).
//   - Delete: remove a local block replica as instructed by NameNode.
// ============================================================================

enum class TaskKind : uint8_t {
    kCopy = 0,
    kDelete = 1,
};

struct DataNodeTask {
    TaskKind kind = TaskKind::kCopy;
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    std::string target_host;  // For copy: destination DN host
    uint32_t target_port = 0; // For copy: destination DN data port
};

// ============================================================================
// ReplicationWorker — executes block replication and deletion tasks.
//
// Tasks are enqueued by the HeartbeatSender (from NN commands) or by the
// BlockReporter (from block report responses). The worker processes them
// sequentially from a thread-safe queue.
//
// For copy tasks, the worker reads block data from local store and sends
// it to the target DN via the pipeline protocol. The actual network send
// is abstracted via a callback (CopyFunc) for testability.
// ============================================================================

using CopyFunc = std::function<pl::Result<pl::Void>(uint64_t block_id,
                                                    uint64_t generation_stamp,
                                                    const std::string& data,
                                                    const std::string& target_host,
                                                    uint32_t target_port)>;

class ReplicationWorker {
public:
    struct Config {
        uint32_t max_concurrent_tasks = 4;
    };

    ReplicationWorker(Config config, LocalBlockStore* store, CopyFunc copy_func);
    ~ReplicationWorker();

    ReplicationWorker(const ReplicationWorker&) = delete;
    ReplicationWorker& operator=(const ReplicationWorker&) = delete;

    /// Start worker threads.
    void start();

    /// Stop worker threads. Blocks until all threads exit.
    void stop();

    /// Enqueue a task for processing.
    void enqueue(DataNodeTask task);

    /// Get the number of pending tasks.
    size_t pending_count() const;

    /// Check if the worker is running.
    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    void worker_loop();
    void execute_copy(const DataNodeTask& task);
    void execute_delete(const DataNodeTask& task);

    Config config_;
    LocalBlockStore* store_;
    CopyFunc copy_func_;

    std::atomic<bool> running_{false};
    mutable std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<DataNodeTask> task_queue_;
    std::vector<std::thread> threads_;
};

} // namespace pl::minidfs
