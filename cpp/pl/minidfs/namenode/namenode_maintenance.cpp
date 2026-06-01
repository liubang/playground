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
// Created: 2026/06/01 23:01

#include "cpp/pl/minidfs/namenode/namenode_maintenance.h"

#include <algorithm>
#include <chrono>
#include <folly/logging/xlog.h>
#include <functional>

#include "cpp/pl/minidfs/common/time_util.h"

namespace pl::minidfs {

NameNodeMaintenance::NameNodeMaintenance(Config config,
                                         MetadataStore* store,
                                         NamespaceManager* namespace_manager,
                                         BlockManager* block_manager,
                                         DataNodeManager* datanode_manager,
                                         ReplicationManager* replication_manager)
    : config_(config),
      store_(store),
      namespace_manager_(namespace_manager),
      block_manager_(block_manager),
      datanode_manager_(datanode_manager),
      replication_manager_(replication_manager) {}

NameNodeMaintenance::~NameNodeMaintenance() {
    stop();
}

size_t NameNodeMaintenance::TaskKeyHash::operator()(const TaskKey& key) const {
    size_t seed = std::hash<uint64_t>{}(key.block_id);
    seed ^= std::hash<uint64_t>{}(key.source_datanode) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= std::hash<uint64_t>{}(key.target_datanode) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

void NameNodeMaintenance::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread([this] { run_loop(); });
}

void NameNodeMaintenance::stop() {
    running_.store(false, std::memory_order_relaxed);
    wait_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

pl::Result<pl::Void> NameNodeMaintenance::recover_expired_leases() {
    auto leases = store_->list_expired_leases(now_ms());
    if (leases.hasError()) {
        return folly::makeUnexpected(leases.error());
    }

    for (const auto& lease : leases.value()) {
        auto txn = store_->begin_transaction();
        if (txn.hasError()) {
            return folly::makeUnexpected(txn.error());
        }
        auto final_length = block_manager_->recover_file(lease.inode_id);
        if (final_length.hasError()) {
            return folly::makeUnexpected(final_length.error());
        }
        auto complete = namespace_manager_->complete_file(lease.inode_id, final_length.value());
        if (complete.hasError()) {
            return folly::makeUnexpected(complete.error());
        }
        auto close = store_->close_lease(lease.inode_id);
        if (close.hasError()) {
            return folly::makeUnexpected(close.error());
        }
        auto commit = txn.value()->commit();
        if (commit.hasError()) {
            return folly::makeUnexpected(commit.error());
        }
        XLOGF(INFO,
              "recovered expired lease {} for inode {}, readable length={}",
              lease.lease_id,
              lease.inode_id,
              final_length.value());
    }
    return pl::Void{};
}

pl::Result<pl::Void> NameNodeMaintenance::scan_datanodes() {
    auto result = datanode_manager_->check_stale_and_dead();
    if (result.hasError()) {
        return folly::makeUnexpected(result.error());
    }
    return pl::Void{};
}

void NameNodeMaintenance::enqueue_replication_tasks(const std::vector<ReplicationTask>& tasks) {
    std::lock_guard lock(task_mu_);
    for (const auto& task : tasks) {
        if (task.is_deletion) {
            continue;
        }
        TaskKey key{task.block_id, task.source_datanode, task.target_datanode};
        if (!queued_tasks_.insert(key).second) {
            continue;
        }
        tasks_by_source_[task.source_datanode].push_back(task);
    }
}

pl::Result<pl::Void> NameNodeMaintenance::scan_replication() {
    auto result = replication_manager_->scan();
    if (result.hasError()) {
        return folly::makeUnexpected(result.error());
    }
    enqueue_replication_tasks(result.value());
    return pl::Void{};
}

pl::Result<pl::Void> NameNodeMaintenance::run_once() {
    auto leases = recover_expired_leases();
    if (leases.hasError()) {
        return leases;
    }
    auto datanodes = scan_datanodes();
    if (datanodes.hasError()) {
        return datanodes;
    }
    return scan_replication();
}

std::vector<ReplicationTask> NameNodeMaintenance::take_replication_tasks(uint64_t source_datanode) {
    std::lock_guard lock(task_mu_);
    auto it = tasks_by_source_.find(source_datanode);
    if (it == tasks_by_source_.end()) {
        return {};
    }
    auto tasks = std::move(it->second);
    tasks_by_source_.erase(it);
    for (const auto& task : tasks) {
        queued_tasks_.erase(TaskKey{task.block_id, task.source_datanode, task.target_datanode});
    }
    return tasks;
}

void NameNodeMaintenance::run_loop() {
    using Clock = std::chrono::steady_clock;
    auto next_lease_recovery = Clock::now();
    auto next_datanode_scan = Clock::now();
    auto next_replication_scan = Clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        auto now = Clock::now();
        if (now >= next_lease_recovery) {
            auto result = recover_expired_leases();
            if (result.hasError()) {
                XLOGF(ERR, "lease recovery scan failed: {}", result.error().describe());
            }
            next_lease_recovery = now + std::chrono::milliseconds(config_.lease_recovery_interval_ms);
        }
        if (now >= next_datanode_scan) {
            auto result = scan_datanodes();
            if (result.hasError()) {
                XLOGF(ERR, "DataNode liveness scan failed: {}", result.error().describe());
            }
            next_datanode_scan = now + std::chrono::milliseconds(config_.datanode_scan_interval_ms);
        }
        if (now >= next_replication_scan) {
            auto result = scan_replication();
            if (result.hasError()) {
                XLOGF(ERR, "replication scan failed: {}", result.error().describe());
            }
            next_replication_scan =
                now + std::chrono::milliseconds(config_.replication_scan_interval_ms);
        }

        auto deadline = std::min({next_lease_recovery, next_datanode_scan, next_replication_scan});
        std::unique_lock lock(wait_mu_);
        wait_cv_.wait_until(lock, deadline, [this] {
            return !running_.load(std::memory_order_relaxed);
        });
    }
}

} // namespace pl::minidfs
