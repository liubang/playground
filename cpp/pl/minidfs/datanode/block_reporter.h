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

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/status/result.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pl::minidfs {

// ============================================================================
// BlockReportResponse — NameNode response to a block report.
//
// The NameNode may instruct the DN to delete blocks it doesn't know about
// (stale replicas), or to re-replicate blocks it considers under-replicated.
// ============================================================================

struct BlockReportResponse {
    std::vector<uint64_t> blocks_to_delete; // block_ids the NN wants removed
};

// ============================================================================
// BlockReporter — periodically reports all stored blocks to the NameNode.
//
// Two modes:
//   - Full report: sends all blocks in current/ (periodic, e.g., every 10min)
//   - Incremental report: sends only blocks added/removed since last report
//     (triggered by finalize/delete events)
//
// The actual RPC is abstracted via a callback for testability.
// ============================================================================

using BlockReportFunc = std::function<pl::Result<BlockReportResponse>(
    uint64_t datanode_id, const std::vector<BlockInfo>& blocks)>;

using DeleteBlockFunc = std::function<void(uint64_t block_id, uint64_t generation_stamp)>;

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

    /// Notify the reporter that a block was finalized (for incremental tracking).
    void notify_block_finalized(uint64_t block_id, uint64_t generation_stamp);

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
