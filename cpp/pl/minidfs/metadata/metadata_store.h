// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/05/10 15:30

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// Transaction — RAII-style database transaction handle.
//
// Usage:
//   auto txn = store->begin_transaction();
//   // ... do work ...
//   txn->commit();
//   // If commit() is not called, destructor rolls back.
class Transaction {
public:
    virtual ~Transaction() = default;

    /// Commit the transaction. Returns error if commit fails.
    virtual pl::Result<pl::Void> commit() = 0;

    /// Explicitly rollback. Also called by destructor if not committed.
    virtual void rollback() = 0;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = default;
    Transaction& operator=(Transaction&&) = default;

protected:
    Transaction() = default;
};

// MetadataStore — abstract interface for all metadata persistence.
//
// Design principles:
//   - All methods return pl::Result<T> for uniform error handling.
//   - The store is thread-safe; implementations must handle concurrency.
//   - Transaction semantics: begin_transaction() returns a Transaction handle;
//     operations within a transaction observe snapshot isolation.
//   - For operations that need transactional guarantees, callers should
//     begin_transaction(), perform operations, then commit().
class MetadataStore {
public:
    virtual ~MetadataStore() = default;

    // Transaction management
    virtual pl::Result<std::unique_ptr<Transaction>> begin_transaction() = 0;

    // Inode operations

    /// Get an inode by ID. Returns NotFound if not exists.
    virtual pl::Result<Inode> get_inode(uint64_t inode_id) = 0;

    /// Get a child inode under parent with the given name.
    /// Returns std::nullopt if the child does not exist (not an error).
    virtual pl::Result<std::optional<Inode>> get_child(uint64_t parent_id,
                                                       std::string_view name) = 0;

    /// List all children of a directory inode.
    virtual pl::Result<std::vector<Inode>> list_children(uint64_t parent_id) = 0;

    /// Create a new inode. Returns AlreadyExists if uk_parent_name conflicts.
    virtual pl::Result<pl::Void> create_inode(const Inode& inode) = 0;

    /// Update an existing inode (matched by inode_id).
    virtual pl::Result<pl::Void> update_inode(const Inode& inode) = 0;

    /// Delete an inode by ID.
    virtual pl::Result<pl::Void> delete_inode(uint64_t inode_id) = 0;

    // Block operations

    /// Get block metadata by block_id.
    virtual pl::Result<BlockMeta> get_block(uint64_t block_id) = 0;

    /// Get all blocks belonging to an inode, ordered by block_index.
    virtual pl::Result<std::vector<BlockMeta>> get_blocks_by_inode(uint64_t inode_id) = 0;

    /// Create a new block metadata entry.
    virtual pl::Result<pl::Void> create_block(const BlockMeta& block) = 0;

    /// Update an existing block metadata entry (matched by block_id).
    virtual pl::Result<pl::Void> update_block(const BlockMeta& block) = 0;

    /// Get all blocks in a given state (used by replication scanner).
    virtual pl::Result<std::vector<BlockMeta>> get_blocks_by_state(BlockState state) = 0;

    // Block Replica operations

    /// Get all replicas for a block.
    virtual pl::Result<std::vector<BlockReplica>> get_replicas(uint64_t block_id) = 0;

    /// Get all replicas on a specific datanode.
    virtual pl::Result<std::vector<BlockReplica>> get_replicas_by_datanode(
        uint64_t datanode_id) = 0;

    /// Insert or update a replica entry (upsert on PK: block_id, datanode_id, storage_id).
    virtual pl::Result<pl::Void> upsert_replica(const BlockReplica& replica) = 0;

    /// Delete all replicas for a given block.
    virtual pl::Result<pl::Void> delete_replicas_by_block(uint64_t block_id) = 0;

    /// Update replica state for a specific (block_id, datanode_id) pair.
    virtual pl::Result<pl::Void> update_replica_state(uint64_t block_id,
                                                      uint64_t datanode_id,
                                                      ReplicaState new_state) = 0;

    // DataNode operations

    /// Get datanode info by ID.
    virtual pl::Result<DataNodeInfo> get_datanode(uint64_t datanode_id) = 0;

    /// Get datanode info by UUID. Returns nullopt if not registered.
    virtual pl::Result<std::optional<DataNodeInfo>> get_datanode_by_uuid(std::string_view uuid) = 0;

    /// List all datanodes in a given state.
    virtual pl::Result<std::vector<DataNodeInfo>> list_datanodes_by_state(DataNodeState state) = 0;

    /// List all datanodes (regardless of state).
    virtual pl::Result<std::vector<DataNodeInfo>> list_all_datanodes() = 0;

    /// Insert or update datanode info (upsert on datanode_id).
    virtual pl::Result<pl::Void> upsert_datanode(const DataNodeInfo& info) = 0;

    // Lease operations

    /// Create a new lease for a file.
    virtual pl::Result<pl::Void> create_lease(const Lease& lease) = 0;

    /// Check if an active lease exists for the given inode.
    virtual pl::Result<std::optional<Lease>> get_active_lease(uint64_t inode_id) = 0;

    /// Renew a lease by updating its expire_time_ms.
    virtual pl::Result<pl::Void> renew_lease(uint64_t inode_id, uint64_t new_expire_ms) = 0;

    /// Close a lease (set state=CLOSED, active_flag=0).
    virtual pl::Result<pl::Void> close_lease(uint64_t inode_id) = 0;

    /// Expire all leases whose expire_time_ms < now_ms. Returns number expired.
    virtual pl::Result<uint64_t> expire_leases(uint64_t now_ms) = 0;

    /// List active leases whose expire_time_ms < now_ms.
    virtual pl::Result<std::vector<Lease>> list_expired_leases(uint64_t now_ms) = 0;

    // ID Allocation

    /// Allocate `count` consecutive IDs from the named allocator.
    /// Returns the first ID in the allocated range.
    virtual pl::Result<uint64_t> alloc_id(std::string_view name, uint64_t count = 1) = 0;

    // Operation Log (for idempotency and auditing)

    /// Write an operation log entry.
    virtual pl::Result<pl::Void> write_oplog(std::string_view op_type,
                                             uint64_t target_inode_id,
                                             std::string_view request_id,
                                             std::string_view payload_json) = 0;

    /// Check if a request_id has already been processed (for idempotency).
    virtual pl::Result<bool> check_request_id(std::string_view request_id) = 0;

    // Lifecycle
    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;
    MetadataStore(MetadataStore&&) = default;
    MetadataStore& operator=(MetadataStore&&) = default;

protected:
    MetadataStore() = default;
};

} // namespace pl::minidfs
