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

#include <algorithm>
#include <random>

#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

PlacementManager::PlacementManager(DataNodeManager* dn_manager) : dn_manager_(dn_manager) {}

pl::Result<std::vector<DataNodeInfo>> PlacementManager::choose_targets(
    uint32_t num_replicas, std::optional<uint64_t> exclude_datanode_id) {
    std::unordered_set<uint64_t> excluded;
    if (exclude_datanode_id.has_value()) {
        excluded.insert(*exclude_datanode_id);
    }
    return choose_targets(num_replicas, excluded);
}

pl::Result<std::vector<DataNodeInfo>> PlacementManager::choose_targets(
    uint32_t num_replicas, const std::unordered_set<uint64_t>& excluded_datanode_ids) {
    auto live_result = dn_manager_->get_live_datanodes();
    if (live_result.hasError()) {
        return folly::makeUnexpected(live_result.error());
    }

    auto& candidates = live_result.value();

    std::erase_if(candidates, [&](const DataNodeInfo& dn) {
        return excluded_datanode_ids.contains(dn.datanode_id);
    });

    if (candidates.size() < num_replicas) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNoAvailableDataNode),
                             "not enough live datanodes for requested replication");
    }

    // Use a thread-local random engine for randomization.
    thread_local std::mt19937 rng = [] {
        std::random_device random_device;
        std::seed_seq seed{
            random_device(), random_device(), random_device(), random_device()};
        return std::mt19937(seed);
    }();

    // Sort by free space descending for capacity-aware placement.
    std::sort(
        candidates.begin(), candidates.end(), [](const DataNodeInfo& a, const DataNodeInfo& b) {
            return a.free_bytes > b.free_bytes;
        });

    // Add bounded randomness: shuffle within the top 2*num_replicas candidates
    // to avoid always picking the exact same nodes while still preferring high-free nodes.
    size_t shuffle_range =
        std::min(candidates.size(), static_cast<size_t>(num_replicas) * 2);
    std::shuffle(candidates.begin(),
                 candidates.begin() +
                     static_cast<std::vector<DataNodeInfo>::difference_type>(shuffle_range),
                 rng);

    // Rack-aware selection.
    std::vector<DataNodeInfo> chosen;
    chosen.reserve(num_replicas);

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
