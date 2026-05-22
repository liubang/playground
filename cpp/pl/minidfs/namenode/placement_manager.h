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
#include <optional>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/namenode/datanode_manager.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// PlacementManager — replica placement policy.
//
// Implements a simplified rack-aware placement:
//   - First replica: random live datanode (or caller's preferred rack)
//   - Second replica: different rack from the first
//   - Third+ replicas: spread across racks, prefer nodes with more free space
//
// Excludes datanodes that are already holding a replica of the same block.
// ============================================================================

class PlacementManager {
public:
    explicit PlacementManager(DataNodeManager* dn_manager);
    ~PlacementManager() = default;

    PlacementManager(const PlacementManager&) = delete;
    PlacementManager& operator=(const PlacementManager&) = delete;

    /// Choose `num_replicas` target datanodes for a new block.
    /// If `exclude_datanode_id` is provided, that node will not be chosen.
    pl::Result<std::vector<DataNodeInfo>> choose_targets(
        uint32_t num_replicas, std::optional<uint64_t> exclude_datanode_id);

private:
    DataNodeManager* dn_manager_;
};

} // namespace pl::minidfs
