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

#include "cpp/pl/minidfs/namenode/lease_manager.h"

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/common/time_util.h"

namespace pl::minidfs {

LeaseManager::LeaseManager(MetadataStore* store) : store_(store) {}

pl::Result<Lease> LeaseManager::acquire_lease(uint64_t inode_id, std::string_view client_id) {
    // Check for existing active lease.
    auto existing = store_->get_active_lease(inode_id);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (existing.value().has_value()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseConflict),
                             "file already has an active lease");
    }

    // Allocate lease ID.
    auto id_result = store_->alloc_id("lease");
    if (id_result.hasError()) {
        return folly::makeUnexpected(id_result.error());
    }

    uint64_t ts = now_ms();
    Lease lease;
    lease.lease_id = id_result.value();
    lease.inode_id = inode_id;
    lease.client_id = std::string(client_id);
    lease.state = LeaseState::kActive;
    lease.expire_time_ms = ts + kDefaultLeaseTimeoutMs;
    lease.ctime_ms = ts;
    lease.mtime_ms = ts;

    auto create_res = store_->create_lease(lease);
    if (create_res.hasError()) {
        return folly::makeUnexpected(create_res.error());
    }
    return lease;
}

pl::Result<pl::Void> LeaseManager::renew_lease(uint64_t inode_id, std::string_view client_id) {
    auto existing = store_->get_active_lease(inode_id);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (!existing.value().has_value()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseNotFound),
                             "no active lease for this file");
    }

    auto& lease = existing.value().value();
    if (lease.client_id != client_id) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseConflict),
                             "lease held by different client");
    }

    uint64_t new_expire = now_ms() + kDefaultLeaseTimeoutMs;
    return store_->renew_lease(inode_id, new_expire);
}

pl::Result<pl::Void> LeaseManager::release_lease(uint64_t inode_id, std::string_view client_id) {
    auto existing = store_->get_active_lease(inode_id);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (!existing.value().has_value()) {
        // No active lease, idempotent.
        return pl::Void{};
    }

    auto& lease = existing.value().value();
    if (lease.client_id != client_id) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseConflict),
                             "lease held by different client");
    }

    return store_->close_lease(inode_id);
}

pl::Result<uint64_t> LeaseManager::expire_stale_leases() {
    return store_->expire_leases(now_ms());
}

pl::Result<bool> LeaseManager::has_active_lease(uint64_t inode_id) {
    auto existing = store_->get_active_lease(inode_id);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    return existing.value().has_value();
}

pl::Result<pl::Void> LeaseManager::validate_lease(uint64_t inode_id, std::string_view client_id) {
    auto existing = store_->get_active_lease(inode_id);
    if (existing.hasError()) {
        return folly::makeUnexpected(existing.error());
    }
    if (!existing.value().has_value()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseNotFound),
                             "no active lease for this file");
    }
    if (existing.value()->client_id != client_id) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseConflict),
                             "lease held by different client");
    }
    return pl::Void{};
}

} // namespace pl::minidfs
