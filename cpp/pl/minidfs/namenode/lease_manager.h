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

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/status/result.h"
#include <cstdint>
#include <string_view>

namespace pl::minidfs {

// ============================================================================
// LeaseManager — manages file write leases.
//
// Only one client can hold a lease on a file at a time. Leases ensure that
// concurrent writers don't corrupt file data. Leases have a timeout and
// must be periodically renewed.
// ============================================================================

class LeaseManager {
public:
    explicit LeaseManager(MetadataStore* store);
    ~LeaseManager() = default;

    LeaseManager(const LeaseManager&) = delete;
    LeaseManager& operator=(const LeaseManager&) = delete;

    /// Acquire a lease on a file for writing. Returns error if lease already held.
    pl::Result<Lease> acquire_lease(uint64_t inode_id, std::string_view client_id);

    /// Renew an existing lease (extend timeout).
    pl::Result<pl::Void> renew_lease(uint64_t inode_id, std::string_view client_id);

    /// Release a lease (file write complete).
    pl::Result<pl::Void> release_lease(uint64_t inode_id, std::string_view client_id);

    /// Run lease expiration scan. Closes expired leases.
    /// Returns the number of leases expired.
    pl::Result<uint64_t> expire_stale_leases();

    /// Check if a file has an active lease.
    pl::Result<bool> has_active_lease(uint64_t inode_id);

private:
    MetadataStore* store_;
};

} // namespace pl::minidfs
