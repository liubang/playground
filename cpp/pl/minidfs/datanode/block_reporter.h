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
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// The NameNode may instruct the DN to delete stale replicas
// or re-replicate under-replicated blocks.
struct BlockDeleteCommand {
    uint64_t block_id = 0;
    uint64_t generation_stamp = 0;
};

struct BlockReportResponse {
    std::vector<BlockDeleteCommand> blocks_to_delete;
};

struct BlockReport {
    bool full_report = false;
    std::vector<BlockInfo> blocks;
};

// BlockReporter — periodically reports all stored blocks to the NameNode.
// Full report: sends all blocks in current/ periodically.
// Incremental tracking: records adds/removes between full reports.
// RPC is abstracted via a callback for testability.
using BlockReportFunc =
    std::function<pl::Result<BlockReportResponse>(uint64_t datanode_id, const BlockReport& report)>;

using DeleteBlockFunc =
    std::function<pl::Result<pl::Void>(uint64_t block_id, uint64_t generation_stamp)>;

class BlockReporter {
public:
    struct Config {
        uint64_t datanode_id = 0;
        uint64_t full_report_interval_ms = kDefaultBlockReportIntervalMs;
    };

    BlockReporter(Config config,
                  LocalBlockStore* store,
                  BlockReportFunc report_func,
                  DeleteBlockFunc delete_func);
    ~BlockReporter();

    BlockReporter(const BlockReporter&) = delete;
    BlockReporter& operator=(const BlockReporter&) = delete;

    /// Start the background reporting thread.
    void start();

    /// Stop the background reporting thread.
    void stop();

    /// Check if the reporter is running.
    bool running() const { return running_.load(std::memory_order_relaxed); }

    /// Send a full block report immediately (useful for startup/testing).
    pl::Result<BlockReportResponse> send_full_report();

    /// Send finalized blocks accumulated since the last successful report.
    pl::Result<BlockReportResponse> send_incremental_report();

    /// Notify the reporter that a block was finalized (for incremental tracking).
    void notify_block_finalized(uint64_t block_id);

    /// Notify the reporter that a block was deleted (for incremental tracking).
    void notify_block_deleted(uint64_t block_id);

private:
    void run_loop();
    void process_response(const BlockReportResponse& response);

    Config config_;
    LocalBlockStore* store_;
    BlockReportFunc report_func_;
    DeleteBlockFunc delete_func_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Incremental tracking
    mutable std::mutex delta_mu_;
    std::unordered_set<uint64_t> added_blocks_;
    std::unordered_set<uint64_t> removed_blocks_;
};

} // namespace pl::minidfs
