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
#include <vector>

#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/minidfs/namenode/placement_manager.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// ReplicationTask — describes a pending replication or deletion action.
// ============================================================================

struct ReplicationTask {
    uint64_t block_id = 0;
    uint64_t source_datanode = 0; // Copy from (0 if deletion task)
    uint64_t target_datanode = 0; // Copy to (0 if deletion task)
    bool is_deletion = false;
};

// ============================================================================
// ReplicationManager — ensures blocks maintain desired replica count.
//
// Periodically scans committed blocks and:
//   - Under-replicated: schedules replication tasks
//   - Over-replicated: schedules deletion tasks
//   - Corrupt replicas: invalidates and schedules re-replication
// ============================================================================

class ReplicationManager {
public:
    ReplicationManager(MetadataStore* store, PlacementManager* placement);
    ~ReplicationManager() = default;

    ReplicationManager(const ReplicationManager&) = delete;
    ReplicationManager& operator=(const ReplicationManager&) = delete;

    /// Scan committed blocks and produce replication/deletion tasks.
    /// Returns the list of tasks to be dispatched to datanodes.
    pl::Result<std::vector<ReplicationTask>> scan();

private:
    MetadataStore* store_;
    PlacementManager* placement_;
};

} // namespace pl::minidfs
