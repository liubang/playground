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
// Created: 2026/05/10 17:30

#include "cpp/pl/minidfs/namenode/datanode_manager.h"

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include <chrono>

namespace pl::minidfs {

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

} // namespace

DataNodeManager::DataNodeManager(MetadataStore* store) : store_(store) {}

pl::Result<uint64_t> DataNodeManager::register_datanode(std::string_view uuid,
                                                        std::string_view hostname,
                                                        std::string_view ip,
                                                        uint32_t rpc_port,
                                                        uint32_t data_port,
                                                        std::string_view rack,
                                                        uint64_t capacity_bytes) {
    // Check if this UUID is already registered.
    auto existing = store_->get_datanode_by_uuid(uuid);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }

    uint64_t ts = now_ms();

    if (existing.value().has_value()) {
        // Re-registration: update info and mark as Live.
        auto& dn = existing.value().value();
        dn.hostname = std::string(hostname);
        dn.ip = std::string(ip);
        dn.rpc_port = rpc_port;
        dn.data_port = data_port;
        dn.rack = std::string(rack);
        dn.state = DataNodeState::kLive;
        dn.capacity_bytes = capacity_bytes;
        dn.last_heartbeat_ms = ts;

        auto upsert_res = store_->upsert_datanode(dn);
        if (upsert_res.hasError()) {
            return folly::makeUnexpected(upsert_res.error());
        }
        return dn.datanode_id;
    }

    // New registration: allocate ID.
    auto id_result = store_->alloc_id("datanode");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    DataNodeInfo dn;
    dn.datanode_id = id_result.value();
    dn.uuid = std::string(uuid);
    dn.hostname = std::string(hostname);
    dn.ip = std::string(ip);
    dn.rpc_port = rpc_port;
    dn.data_port = data_port;
    dn.rack = std::string(rack);
    dn.state = DataNodeState::kLive;
    dn.capacity_bytes = capacity_bytes;
    dn.used_bytes = 0;
    dn.free_bytes = capacity_bytes;
    dn.last_heartbeat_ms = ts;

    auto upsert_res = store_->upsert_datanode(dn);
    if (upsert_res.hasError()) {
        return folly::makeUnexpected(upsert_res.error());
    }
    return dn.datanode_id;
}

pl::Result<pl::Void> DataNodeManager::handle_heartbeat(uint64_t datanode_id,
                                                       uint64_t capacity_bytes,
                                                       uint64_t used_bytes,
                                                       uint64_t free_bytes) {
    auto dn_result = store_->get_datanode(datanode_id);
    if (dn_result.hasError()) {
        return folly::makeUnexpected(dn_result.error());
    }
    auto& dn = dn_result.value();

    dn.state = DataNodeState::kLive;
    dn.capacity_bytes = capacity_bytes;
    dn.used_bytes = used_bytes;
    dn.free_bytes = free_bytes;
    dn.last_heartbeat_ms = now_ms();

    return store_->upsert_datanode(dn);
}

pl::Result<uint32_t> DataNodeManager::check_stale_and_dead() {
    auto all_dns = store_->list_all_datanodes();
    if (all_dns.hasError()) {
        return folly::makeUnexpected(all_dns.error());
    }

    uint64_t ts = now_ms();
    uint32_t changed = 0;

    for (auto& dn : all_dns.value()) {
        if (dn.state == DataNodeState::kDecommissioning ||
            dn.state == DataNodeState::kDecommissioned) {
            continue; // Don't touch decommissioning nodes.
        }

        uint64_t elapsed = ts - dn.last_heartbeat_ms;
        DataNodeState new_state = dn.state;

        if (elapsed >= kDefaultDeadTimeoutMs) {
            new_state = DataNodeState::kDead;
        } else if (elapsed >= kDefaultStaleTimeoutMs) {
            new_state = DataNodeState::kStale;
        } else {
            new_state = DataNodeState::kLive;
        }

        if (new_state != dn.state) {
            dn.state = new_state;
            auto res = store_->upsert_datanode(dn);
            if (res.hasError()) {
                return folly::makeUnexpected(res.error());
            }
            ++changed;
        }
    }
    return changed;
}

pl::Result<std::vector<DataNodeInfo>> DataNodeManager::get_live_datanodes() {
    return store_->list_datanodes_by_state(DataNodeState::kLive);
}

pl::Result<std::vector<DataNodeInfo>> DataNodeManager::get_all_datanodes() {
    return store_->list_all_datanodes();
}

pl::Result<DataNodeInfo> DataNodeManager::get_datanode(uint64_t datanode_id) {
    return store_->get_datanode(datanode_id);
}

} // namespace pl::minidfs
