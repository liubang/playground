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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/minidfs/namenode/block_manager.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/minidfs/namenode/namespace_manager.h"
#include "cpp/pl/minidfs/namenode/replication_manager.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// NameNodeMaintenance closes the control-plane loops that do not belong in
// RPC handlers: lease recovery, DataNode liveness, and replica repair.
class NameNodeMaintenance {
public:
    struct Config {
        uint64_t lease_recovery_interval_ms = 1000;
        uint64_t datanode_scan_interval_ms = kDefaultHeartbeatIntervalMs;
        uint64_t replication_scan_interval_ms = kDefaultReplicationScanIntervalMs;
    };

    NameNodeMaintenance(Config config,
                        MetadataStore* store,
                        NamespaceManager* namespace_manager,
                        BlockManager* block_manager,
                        DataNodeManager* datanode_manager,
                        ReplicationManager* replication_manager);
    ~NameNodeMaintenance();

    NameNodeMaintenance(const NameNodeMaintenance&) = delete;
    NameNodeMaintenance& operator=(const NameNodeMaintenance&) = delete;

    void start();
    void stop();

    /// Run every maintenance action immediately. Intended for startup and tests.
    pl::Result<pl::Void> run_once();

    /// Drain replication tasks assigned to a source DataNode.
    std::vector<ReplicationTask> take_replication_tasks(uint64_t source_datanode);

private:
    struct TaskKey {
        uint64_t block_id = 0;
        uint64_t source_datanode = 0;
        uint64_t target_datanode = 0;

        bool operator==(const TaskKey&) const = default;
    };

    struct TaskKeyHash {
        size_t operator()(const TaskKey& key) const;
    };

    pl::Result<pl::Void> recover_expired_leases();
    pl::Result<pl::Void> scan_datanodes();
    pl::Result<pl::Void> scan_replication();
    void enqueue_replication_tasks(const std::vector<ReplicationTask>& tasks);
    void run_loop();

    Config config_;
    MetadataStore* store_;
    NamespaceManager* namespace_manager_;
    BlockManager* block_manager_;
    DataNodeManager* datanode_manager_;
    ReplicationManager* replication_manager_;

    std::atomic<bool> running_{false};
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    std::thread thread_;

    std::mutex task_mu_;
    std::unordered_map<uint64_t, std::vector<ReplicationTask>> tasks_by_source_;
    std::unordered_set<TaskKey, TaskKeyHash> queued_tasks_;
};

} // namespace pl::minidfs
