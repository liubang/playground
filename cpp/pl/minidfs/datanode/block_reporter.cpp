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

#include <butil/logging.h>
#include <chrono>

namespace pl::minidfs {

BlockReporter::BlockReporter(Config config,
                             LocalBlockStore* store,
                             BlockReportFunc report_func,
                             DeleteBlockFunc delete_func)
    : config_(config),
      store_(store),
      report_func_(std::move(report_func)),
      delete_func_(std::move(delete_func)) {}

BlockReporter::~BlockReporter() {
    stop();
}

void BlockReporter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread([this] { run_loop(); });
}

void BlockReporter::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

pl::Result<BlockReportResponse> BlockReporter::send_full_report() {
    auto blocks_result = store_->report_blocks();
    if (blocks_result.hasError()) {
        LOG(ERROR) << "failed to enumerate blocks for report: " << blocks_result.error().describe();
        return pl::makeError(std::move(blocks_result.error()));
    }

    BlockReport report{
        .full_report = true,
        .blocks = std::move(blocks_result.value()),
    };
    auto response = report_func_(config_.datanode_id, report);
    if (response.hasError()) {
        LOG(WARNING) << "block report RPC failed for datanode " << config_.datanode_id << ": "
                     << response.error().describe();
        return pl::makeError(std::move(response.error()));
    }

    // Process NN commands
    process_response(response.value());

    // Clear only blocks covered by this report. A block may finalize while the
    // RPC is in flight and must remain pending for the next incremental report.
    {
        std::lock_guard lock(delta_mu_);
        for (const auto& block : report.blocks) {
            added_blocks_.erase(block.block_id);
        }
        removed_blocks_.clear();
    }

    return std::move(response.value());
}

pl::Result<BlockReportResponse> BlockReporter::send_incremental_report() {
    std::unordered_set<uint64_t> added_blocks;
    {
        std::lock_guard lock(delta_mu_);
        added_blocks = added_blocks_;
    }
    if (added_blocks.empty()) {
        return BlockReportResponse{};
    }

    auto blocks_result = store_->report_blocks();
    if (blocks_result.hasError()) {
        return pl::makeError(std::move(blocks_result.error()));
    }

    BlockReport report;
    std::vector<uint64_t> reported_block_ids;
    for (auto& block : blocks_result.value()) {
        if (added_blocks.contains(block.block_id)) {
            reported_block_ids.push_back(block.block_id);
            report.blocks.push_back(block);
        }
    }
    if (report.blocks.empty()) {
        return BlockReportResponse{};
    }
    auto response = report_func_(config_.datanode_id, report);
    if (response.hasError()) {
        return pl::makeError(std::move(response.error()));
    }

    process_response(response.value());
    {
        std::lock_guard lock(delta_mu_);
        for (uint64_t block_id : reported_block_ids) {
            added_blocks_.erase(block_id);
        }
    }
    return std::move(response.value());
}

void BlockReporter::notify_block_finalized(uint64_t block_id) {
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
    for (const auto& cmd : response.blocks_to_delete) {
        if (!delete_func_) {
            continue;
        }
        auto deleted = delete_func_(cmd.block_id, cmd.generation_stamp);
        if (deleted.hasError()) {
            LOG(WARNING) << "failed to apply NN delete command for block " << cmd.block_id << ":"
                         << cmd.generation_stamp << ": " << deleted.error().describe();
            continue;
        }
        notify_block_deleted(cmd.block_id);
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
            (void)send_incremental_report();
        }

        if (!running_.load(std::memory_order_relaxed)) {
            break;
        }

        send_full_report();
    }
}

} // namespace pl::minidfs
