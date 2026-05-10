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

#include "cpp/pl/minidfs/datanode/block_reporter.h"

#include <chrono>
#include <folly/logging/xlog.h>

namespace pl::minidfs {

// ============================================================================
// Construction & Destruction
// ============================================================================

BlockReporter::BlockReporter(Config config,
                             LocalBlockStore* store,
                             BlockReportFunc report_func,
                             DeleteBlockFunc delete_func)
    : config_(std::move(config)),
      store_(store),
      report_func_(std::move(report_func)),
      delete_func_(std::move(delete_func)) {}

BlockReporter::~BlockReporter() { stop(); }

// ============================================================================
// Lifecycle
// ============================================================================

void BlockReporter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread([this] {
        run_loop();
    });
}

void BlockReporter::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

// ============================================================================
// Reporting Logic
// ============================================================================

pl::Result<BlockReportResponse> BlockReporter::send_full_report() {
    auto blocks_result = store_->report_blocks();
    if (blocks_result.hasError()) {
        XLOGF(ERR, "failed to enumerate blocks for report: {}", blocks_result.error().describe());
        return pl::makeError(std::move(blocks_result.error()));
    }

    auto response = report_func_(config_.datanode_id, blocks_result.value());
    if (response.hasError()) {
        XLOGF(WARN, "block report RPC failed for datanode {}: {}", config_.datanode_id,
              response.error().describe());
        return pl::makeError(std::move(response.error()));
    }

    // Process NN commands
    process_response(response.value());

    // Clear incremental delta after a successful full report
    {
        std::lock_guard lock(delta_mu_);
        added_blocks_.clear();
        removed_blocks_.clear();
    }

    return std::move(response.value());
}

void BlockReporter::notify_block_finalized(uint64_t block_id,
                                           [[maybe_unused]] uint64_t generation_stamp) {
    std::lock_guard lock(delta_mu_);
    removed_blocks_.erase(block_id);
    added_blocks_.insert(block_id);
}

void BlockReporter::notify_block_deleted(uint64_t block_id) {
    std::lock_guard lock(delta_mu_);
    added_blocks_.erase(block_id);
    removed_blocks_.insert(block_id);
}

void BlockReporter::process_response(const BlockReportResponse& response) {
    for (uint64_t block_id : response.blocks_to_delete) {
        if (delete_func_) {
            // We don't have the generation_stamp from the response here,
            // so the delete_func should handle lookup by block_id.
            delete_func_(block_id, 0);
        }
    }
}

void BlockReporter::run_loop() {
    // Send initial full report on startup
    send_full_report();

    while (running_.load(std::memory_order_relaxed)) {
        // Sleep until next full report interval
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config_.full_report_interval_ms);
        while (running_.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }

        send_full_report();
    }
}

} // namespace pl::minidfs
