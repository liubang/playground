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

#include "cpp/pl/minidfs/namenode/placement_manager.h"

#include "cpp/pl/minidfs/common/error_code.h"
#include <algorithm>
#include <random>

namespace pl::minidfs {

PlacementManager::PlacementManager(DataNodeManager* dn_manager) : dn_manager_(dn_manager) {}

pl::Result<std::vector<DataNodeInfo>> PlacementManager::choose_targets(
    uint32_t num_replicas, std::optional<uint64_t> exclude_datanode_id) {

    auto live_result = dn_manager_->get_live_datanodes();
    if (live_result.hasError()) {
        return folly::makeUnexpected(live_result.error());
    }

    auto& candidates = live_result.value();

    // Filter out excluded datanode.
    if (exclude_datanode_id.has_value()) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const DataNodeInfo& dn) {
                                            return dn.datanode_id == exclude_datanode_id.value();
                                        }),
                         candidates.end());
    }

    if (candidates.size() < num_replicas) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNoAvailableDataNode),
                             "not enough live datanodes for requested replication");
    }

    // Sort by free space descending, then shuffle within each rack group
    // for simple rack-awareness.
    std::sort(candidates.begin(), candidates.end(),
              [](const DataNodeInfo& a, const DataNodeInfo& b) {
                  return a.free_bytes > b.free_bytes;
              });

    // Simple rack-aware selection:
    // 1. Pick the first node (most free space).
    // 2. Try to pick a node from a different rack.
    // 3. Fill remaining from remaining candidates.
    std::vector<DataNodeInfo> chosen;
    chosen.reserve(num_replicas);

    // Use a thread-local random engine for shuffling.
    thread_local std::mt19937 rng{std::random_device{}()};

    // Shuffle to add some randomness (avoid always picking the same top-free nodes).
    std::shuffle(candidates.begin(), candidates.end(), rng);

    // Greedy rack-aware selection.
    std::vector<std::string> used_racks;

    for (const auto& dn : candidates) {
        if (chosen.size() >= num_replicas) {
            break;
        }

        // For the second replica, prefer a different rack.
        if (chosen.size() == 1) {
            bool same_rack = (dn.rack == chosen[0].rack);
            // If there are candidates from other racks, skip same-rack ones.
            if (same_rack && candidates.size() > num_replicas) {
                // Check if there are other-rack options remaining.
                bool has_other_rack = false;
                for (size_t i = 0; i < candidates.size(); ++i) {
                    if (candidates[i].rack != chosen[0].rack &&
                        candidates[i].datanode_id != chosen[0].datanode_id) {
                        has_other_rack = true;
                        break;
                    }
                }
                if (has_other_rack) {
                    continue; // Skip this same-rack candidate.
                }
            }
        }

        chosen.push_back(dn);
        used_racks.push_back(dn.rack);
    }

    // If rack-awareness skipped too many, fill greedily.
    if (chosen.size() < num_replicas) {
        for (const auto& dn : candidates) {
            if (chosen.size() >= num_replicas) {
                break;
            }
            bool already_chosen = false;
            for (const auto& c : chosen) {
                if (c.datanode_id == dn.datanode_id) {
                    already_chosen = true;
                    break;
                }
            }
            if (!already_chosen) {
                chosen.push_back(dn);
            }
        }
    }

    if (chosen.size() < num_replicas) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNoAvailableDataNode),
                             "could not select enough distinct datanodes");
    }

    return chosen;
}

} // namespace pl::minidfs
