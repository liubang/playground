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

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// DataNodeManager — manages datanode registration and heartbeat state machine.
//
// State transitions:
//   (new) -> Live -> Stale -> Dead
//   Live: heartbeat received within stale timeout
//   Stale: no heartbeat for stale_timeout_ms
//   Dead: no heartbeat for dead_timeout_ms
// ============================================================================

class DataNodeManager {
public:
    explicit DataNodeManager(MetadataStore* store);
    ~DataNodeManager() = default;

    DataNodeManager(const DataNodeManager&) = delete;
    DataNodeManager& operator=(const DataNodeManager&) = delete;

    /// Register a new datanode or re-register an existing one.
    /// Returns the assigned datanode_id.
    pl::Result<uint64_t> register_datanode(std::string_view uuid,
                                           std::string_view hostname,
                                           std::string_view ip,
                                           uint32_t rpc_port,
                                           uint32_t data_port,
                                           std::string_view rack,
                                           uint64_t capacity_bytes);

    /// Process a heartbeat from a datanode. Updates last_heartbeat_ms and capacity info.
    pl::Result<pl::Void> handle_heartbeat(uint64_t datanode_id,
                                          uint64_t capacity_bytes,
                                          uint64_t used_bytes,
                                          uint64_t free_bytes);

    /// Run a scan to transition datanodes from Live->Stale->Dead based on heartbeat age.
    /// Returns the number of state transitions performed.
    pl::Result<uint32_t> check_stale_and_dead();

    /// Get all live datanodes (available for block placement).
    pl::Result<std::vector<DataNodeInfo>> get_live_datanodes();

    /// Get all datanodes regardless of state.
    pl::Result<std::vector<DataNodeInfo>> get_all_datanodes();

    /// Get a specific datanode by ID.
    pl::Result<DataNodeInfo> get_datanode(uint64_t datanode_id);

private:
    MetadataStore* store_;
};

} // namespace pl::minidfs
