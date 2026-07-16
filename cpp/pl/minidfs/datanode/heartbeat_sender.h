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

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

enum class CommandType : uint8_t {
    kNone = 0,
    kReplicate = 1,  // Replicate a block to another DN
    kDelete = 2,     // Delete a block replica
    kInvalidate = 3, // Invalidate and re-report
    kShutdown = 4,   // Graceful shutdown requested
};

struct HeartbeatCommand {
    CommandType type = CommandType::kNone;
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
    uint64_t inode_id = 0;
    uint32_t block_index = 0;
    std::string target_host; // For replication: destination DN
    uint32_t target_port = 0;
    BlockToken block_token;
};

// HeartbeatSender — periodically sends heartbeats to the NameNode.
// The NameNode responds with commands (replicate, delete, etc.) dispatched
// to the appropriate workers. RPC is abstracted via HeartbeatFunc for testability.
using HeartbeatFunc = std::function<pl::Result<std::vector<HeartbeatCommand>>(
    uint64_t datanode_id, uint64_t capacity_bytes, uint64_t used_bytes, uint64_t free_bytes)>;

using CommandHandler = std::function<void(const HeartbeatCommand& cmd)>;

class HeartbeatSender {
public:
    struct Config {
        uint64_t datanode_id = 0;
        uint64_t interval_ms = kDefaultHeartbeatIntervalMs;
    };

    HeartbeatSender(Config config,
                    LocalBlockStore* store,
                    HeartbeatFunc heartbeat_func,
                    CommandHandler command_handler);
    ~HeartbeatSender();

    HeartbeatSender(const HeartbeatSender&) = delete;
    HeartbeatSender& operator=(const HeartbeatSender&) = delete;

    /// Start the heartbeat background thread.
    void start();

    /// Stop the heartbeat background thread. Blocks until thread exits.
    void stop();

    /// Check if the sender is running.
    bool running() const { return running_.load(std::memory_order_relaxed); }

    /// Send one heartbeat immediately (useful for testing).
    pl::Result<std::vector<HeartbeatCommand>> send_once();

private:
    void run_loop();

    Config config_;
    LocalBlockStore* store_;
    HeartbeatFunc heartbeat_func_;
    CommandHandler command_handler_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace pl::minidfs
