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
// Created: 2026/05/10 21:00

#pragma once

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/metadata/metadata_store.h"

namespace pl::minidfs::testing {

// ============================================================================
// MockTransaction — no-op transaction for in-memory store.
// ============================================================================

class MockTransaction final : public Transaction {
public:
    pl::Result<pl::Void> commit() override { return pl::Void{}; }
    void rollback() override {}
};

// ============================================================================
// MockMetadataStore — thread-safe in-memory MetadataStore for unit tests.
//
// Provides a fully functional implementation backed by std::unordered_map,
// enabling all Manager tests to run without MySQL.
// ============================================================================

class MockMetadataStore final : public MetadataStore {
public:
    MockMetadataStore() {
        // Initialize root inode.
        Inode root;
        root.inode_id = 1;
        root.type = InodeType::kDirectory;
        root.parent_id = 0;
        root.name = "/";
        root.owner = "root";
        root.group = "supergroup";
        root.permission = 0755;
        root.ctime_ms = 1;
        root.mtime_ms = 1;
        root.version = 1;
        inodes_[root.inode_id] = root;

        // Initialize ID allocators.
        id_counters_["inode"] = 1000;
        id_counters_["block"] = 1000;
        id_counters_["datanode"] = 1000;
        id_counters_["lease"] = 1000;
    }

    // ========================================================================
    // Transaction
    // ========================================================================

    pl::Result<std::unique_ptr<Transaction>> begin_transaction() override {
        return std::unique_ptr<Transaction>(std::make_unique<MockTransaction>());
    }

    // ========================================================================
    // Inode operations
    // ========================================================================

    pl::Result<Inode> get_inode(uint64_t inode_id) override {
        std::lock_guard lock(mu_);
        auto it = inodes_.find(inode_id);
        if (it == inodes_.end()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                                 "inode not found");
        }
        return it->second;
    }

    pl::Result<std::optional<Inode>> get_child(uint64_t parent_id, std::string_view name) override {
        std::lock_guard lock(mu_);
        for (const auto& [_, inode] : inodes_) {
            if (inode.parent_id == parent_id && inode.name == name) {
                return std::optional<Inode>(inode);
            }
        }
        return std::optional<Inode>(std::nullopt);
    }

    pl::Result<std::vector<Inode>> list_children(uint64_t parent_id) override {
        std::lock_guard lock(mu_);
        std::vector<Inode> children;
        for (const auto& [_, inode] : inodes_) {
            if (inode.parent_id == parent_id && inode.inode_id != 1) {
                children.push_back(inode);
            }
        }
        return children;
    }

    pl::Result<pl::Void> create_inode(const Inode& inode) override {
        std::lock_guard lock(mu_);
        // Check unique constraint (parent_id, name).
        for (const auto& [_, existing] : inodes_) {
            if (existing.parent_id == inode.parent_id && existing.name == inode.name) {
                return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kAlreadyExists),
                                     "inode already exists");
            }
        }
        inodes_[inode.inode_id] = inode;
        return pl::Void{};
    }

    pl::Result<pl::Void> update_inode(const Inode& inode) override {
        std::lock_guard lock(mu_);
        auto it = inodes_.find(inode.inode_id);
        if (it == inodes_.end()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                                 "inode not found");
        }
        it->second = inode;
        return pl::Void{};
    }

    pl::Result<pl::Void> delete_inode(uint64_t inode_id) override {
        std::lock_guard lock(mu_);
        inodes_.erase(inode_id);
        return pl::Void{};
    }

    // ========================================================================
    // Block operations
    // ========================================================================

    pl::Result<BlockMeta> get_block(uint64_t block_id) override {
        std::lock_guard lock(mu_);
        auto it = blocks_.find(block_id);
        if (it == blocks_.end()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kBlockNotFound),
                                 "block not found");
        }
        return it->second;
    }

    pl::Result<std::vector<BlockMeta>> get_blocks_by_inode(uint64_t inode_id) override {
        std::lock_guard lock(mu_);
        std::vector<BlockMeta> result;
        for (const auto& [_, block] : blocks_) {
            if (block.inode_id == inode_id) {
                result.push_back(block);
            }
        }
        std::sort(result.begin(), result.end(), [](const BlockMeta& a, const BlockMeta& b) {
            return a.block_index < b.block_index;
        });
        return result;
    }

    pl::Result<pl::Void> create_block(const BlockMeta& block) override {
        std::lock_guard lock(mu_);
        blocks_[block.block_id] = block;
        return pl::Void{};
    }

    pl::Result<pl::Void> update_block(const BlockMeta& block) override {
        std::lock_guard lock(mu_);
        auto it = blocks_.find(block.block_id);
        if (it == blocks_.end()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kBlockNotFound),
                                 "block not found");
        }
        it->second = block;
        return pl::Void{};
    }

    pl::Result<std::vector<BlockMeta>> get_blocks_by_state(BlockState state) override {
        std::lock_guard lock(mu_);
        std::vector<BlockMeta> result;
        for (const auto& [_, block] : blocks_) {
            if (block.state == state) {
                result.push_back(block);
            }
        }
        return result;
    }

    // ========================================================================
    // Block Replica operations
    // ========================================================================

    pl::Result<std::vector<BlockReplica>> get_replicas(uint64_t block_id) override {
        std::lock_guard lock(mu_);
        std::vector<BlockReplica> result;
        for (const auto& r : replicas_) {
            if (r.block_id == block_id) {
                result.push_back(r);
            }
        }
        return result;
    }

    pl::Result<std::vector<BlockReplica>> get_replicas_by_datanode(uint64_t datanode_id) override {
        std::lock_guard lock(mu_);
        std::vector<BlockReplica> result;
        for (const auto& r : replicas_) {
            if (r.datanode_id == datanode_id) {
                result.push_back(r);
            }
        }
        return result;
    }

    pl::Result<pl::Void> upsert_replica(const BlockReplica& replica) override {
        std::lock_guard lock(mu_);
        for (auto& r : replicas_) {
            if (r.block_id == replica.block_id && r.datanode_id == replica.datanode_id &&
                r.storage_id == replica.storage_id) {
                r = replica;
                return pl::Void{};
            }
        }
        replicas_.push_back(replica);
        return pl::Void{};
    }

    pl::Result<pl::Void> delete_replicas_by_block(uint64_t block_id) override {
        std::lock_guard lock(mu_);
        replicas_.erase(
            std::remove_if(replicas_.begin(),
                           replicas_.end(),
                           [block_id](const BlockReplica& r) { return r.block_id == block_id; }),
            replicas_.end());
        return pl::Void{};
    }

    pl::Result<pl::Void> update_replica_state(uint64_t block_id,
                                              uint64_t datanode_id,
                                              ReplicaState new_state) override {
        std::lock_guard lock(mu_);
        for (auto& r : replicas_) {
            if (r.block_id == block_id && r.datanode_id == datanode_id) {
                r.state = new_state;
                return pl::Void{};
            }
        }
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kReplicaNotFound),
                             "replica not found");
    }

    // ========================================================================
    // DataNode operations
    // ========================================================================

    pl::Result<DataNodeInfo> get_datanode(uint64_t datanode_id) override {
        std::lock_guard lock(mu_);
        auto it = datanodes_.find(datanode_id);
        if (it == datanodes_.end()) {
            return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                                 "datanode not found");
        }
        return it->second;
    }

    pl::Result<std::optional<DataNodeInfo>> get_datanode_by_uuid(std::string_view uuid) override {
        std::lock_guard lock(mu_);
        for (const auto& [_, dn] : datanodes_) {
            if (dn.uuid == uuid) {
                return std::optional<DataNodeInfo>(dn);
            }
        }
        return std::optional<DataNodeInfo>(std::nullopt);
    }

    pl::Result<std::vector<DataNodeInfo>> list_datanodes_by_state(DataNodeState state) override {
        std::lock_guard lock(mu_);
        std::vector<DataNodeInfo> result;
        for (const auto& [_, dn] : datanodes_) {
            if (dn.state == state) {
                result.push_back(dn);
            }
        }
        return result;
    }

    pl::Result<std::vector<DataNodeInfo>> list_all_datanodes() override {
        std::lock_guard lock(mu_);
        std::vector<DataNodeInfo> result;
        result.reserve(datanodes_.size());
        for (const auto& [_, dn] : datanodes_) {
            result.push_back(dn);
        }
        return result;
    }

    pl::Result<pl::Void> upsert_datanode(const DataNodeInfo& info) override {
        std::lock_guard lock(mu_);
        datanodes_[info.datanode_id] = info;
        return pl::Void{};
    }

    // ========================================================================
    // Lease operations
    // ========================================================================

    pl::Result<pl::Void> create_lease(const Lease& lease) override {
        std::lock_guard lock(mu_);
        leases_.push_back(lease);
        return pl::Void{};
    }

    pl::Result<std::optional<Lease>> get_active_lease(uint64_t inode_id) override {
        std::lock_guard lock(mu_);
        for (const auto& l : leases_) {
            if (l.inode_id == inode_id && l.state == LeaseState::kActive) {
                return std::optional<Lease>(l);
            }
        }
        return std::optional<Lease>(std::nullopt);
    }

    pl::Result<pl::Void> renew_lease(uint64_t inode_id, uint64_t new_expire_ms) override {
        std::lock_guard lock(mu_);
        for (auto& l : leases_) {
            if (l.inode_id == inode_id && l.state == LeaseState::kActive) {
                l.expire_time_ms = new_expire_ms;
                return pl::Void{};
            }
        }
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kLeaseNotFound),
                             "no active lease");
    }

    pl::Result<pl::Void> close_lease(uint64_t inode_id) override {
        std::lock_guard lock(mu_);
        for (auto& l : leases_) {
            if (l.inode_id == inode_id && l.state == LeaseState::kActive) {
                l.state = LeaseState::kClosed;
                return pl::Void{};
            }
        }
        return pl::Void{};
    }

    pl::Result<uint64_t> expire_leases(uint64_t now_ms) override {
        std::lock_guard lock(mu_);
        uint64_t count = 0;
        for (auto& l : leases_) {
            if (l.state == LeaseState::kActive && l.expire_time_ms < now_ms) {
                l.state = LeaseState::kClosed;
                ++count;
            }
        }
        return count;
    }

    // ========================================================================
    // ID Allocation
    // ========================================================================

    pl::Result<uint64_t> alloc_id(std::string_view name, uint64_t count = 1) override {
        std::lock_guard lock(mu_);
        auto key = std::string(name);
        auto it = id_counters_.find(key);
        if (it == id_counters_.end()) {
            id_counters_[key] = 1000;
            it = id_counters_.find(key);
        }
        uint64_t id = it->second;
        it->second += count;
        return id;
    }

    // ========================================================================
    // Operation Log
    // ========================================================================

    pl::Result<pl::Void> write_oplog(std::string_view /*op_type*/,
                                     uint64_t /*target_inode_id*/,
                                     std::string_view request_id,
                                     std::string_view /*payload_json*/) override {
        std::lock_guard lock(mu_);
        request_ids_.emplace(request_id);
        return pl::Void{};
    }

    pl::Result<bool> check_request_id(std::string_view request_id) override {
        std::lock_guard lock(mu_);
        return request_ids_.count(std::string(request_id)) > 0;
    }

private:
    std::mutex mu_;
    std::unordered_map<uint64_t, Inode> inodes_;
    std::unordered_map<uint64_t, BlockMeta> blocks_;
    std::vector<BlockReplica> replicas_;
    std::unordered_map<uint64_t, DataNodeInfo> datanodes_;
    std::vector<Lease> leases_;
    std::unordered_map<std::string, uint64_t> id_counters_;
    std::unordered_set<std::string> request_ids_;
};

} // namespace pl::minidfs::testing
