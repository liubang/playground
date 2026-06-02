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

#include "cpp/pl/minidfs/datanode/heartbeat_sender.h"

#include <butil/logging.h>
#include <chrono>

namespace pl::minidfs {

HeartbeatSender::HeartbeatSender(Config config,
                                 LocalBlockStore* store,
                                 HeartbeatFunc heartbeat_func,
                                 CommandHandler command_handler)
    : config_(std::move(config)),
      store_(store),
      heartbeat_func_(std::move(heartbeat_func)),
      command_handler_(std::move(command_handler)) {}

HeartbeatSender::~HeartbeatSender() {
    stop();
}

void HeartbeatSender::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // Already running
    }
    thread_ = std::thread([this] { run_loop(); });
}

void HeartbeatSender::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

pl::Result<std::vector<HeartbeatCommand>> HeartbeatSender::send_once() {
    // Query storage capacity from local block store
    auto avail_result = store_->available_bytes();
    uint64_t free_bytes = avail_result.hasValue() ? avail_result.value() : 0;

    // For capacity and used, we derive from filesystem info.
    // available_bytes already subtracts reserved, so we use raw fs::space.
    std::error_code ec;
    auto space = std::filesystem::space(store_->storage_root(), ec);
    uint64_t capacity_bytes = ec ? 0 : space.capacity;
    uint64_t used_bytes = ec ? 0 : (space.capacity - space.available);

    // Send heartbeat via the provided RPC function
    auto result = heartbeat_func_(config_.datanode_id, capacity_bytes, used_bytes, free_bytes);
    if (result.hasError()) {
        LOG(WARNING) << "heartbeat failed for datanode " << config_.datanode_id << ": "
                     << result.error().describe();
        return pl::makeError(std::move(result.error()));
    }

    // Dispatch received commands
    for (const auto& cmd : result.value()) {
        if (cmd.type != CommandType::kNone && command_handler_) {
            command_handler_(cmd);
        }
    }

    return std::move(result.value());
}

void HeartbeatSender::run_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        send_once(); // Ignore errors in background loop; logged inside send_once.

        // Sleep with periodic wake-up check for faster shutdown
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.interval_ms);
        while (running_.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace pl::minidfs
