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
// Created: 2026/05/10 17:29

#pragma once

#include <memory>

#include "cpp/pl/minidfs/metadata/metadata_store.h"
#include "cpp/pl/minidfs/metadata/mysql_connection_pool.h"

namespace pl::minidfs {

// MySQLMetadataStore — concrete MetadataStore backed by MySQL via boost.mysql.
//
// Thread-safe: each operation acquires a connection from the pool.

class MySQLMetadataStore final : public MetadataStore {
public:
    ~MySQLMetadataStore() override = default;

    /// Factory method. Creates the store with the given connection pool.
    static pl::Result<std::unique_ptr<MySQLMetadataStore>> create(
        std::shared_ptr<MySQLConnectionPool> pool);

    // Transaction management

    pl::Result<std::unique_ptr<Transaction>> begin_transaction() override;

    // Inode operations

    pl::Result<Inode> get_inode(uint64_t inode_id) override;
    pl::Result<std::optional<Inode>> get_child(uint64_t parent_id, std::string_view name) override;
    pl::Result<std::vector<Inode>> list_children(uint64_t parent_id) override;
    pl::Result<pl::Void> create_inode(const Inode& inode) override;
    pl::Result<pl::Void> update_inode(const Inode& inode) override;
    pl::Result<pl::Void> delete_inode(uint64_t inode_id) override;

    // Block operations

    pl::Result<BlockMeta> get_block(uint64_t block_id) override;
    pl::Result<std::vector<BlockMeta>> get_blocks_by_inode(uint64_t inode_id) override;
    pl::Result<pl::Void> create_block(const BlockMeta& block) override;
    pl::Result<pl::Void> delete_block(uint64_t block_id) override;
    pl::Result<pl::Void> update_block(const BlockMeta& block) override;
    pl::Result<std::vector<BlockMeta>> get_blocks_by_state(BlockState state) override;

    // Block Replica operations

    pl::Result<std::vector<BlockReplica>> get_replicas(uint64_t block_id) override;
    pl::Result<std::vector<BlockReplica>> get_replicas_by_datanode(uint64_t datanode_id) override;
    pl::Result<pl::Void> upsert_replica(const BlockReplica& replica) override;
    pl::Result<pl::Void> delete_replicas_by_block(uint64_t block_id) override;
    pl::Result<pl::Void> update_replica_state(uint64_t block_id,
                                              uint64_t datanode_id,
                                              ReplicaState new_state) override;

    // DataNode operations

    pl::Result<DataNodeInfo> get_datanode(uint64_t datanode_id) override;
    pl::Result<std::optional<DataNodeInfo>> get_datanode_by_uuid(std::string_view uuid) override;
    pl::Result<std::vector<DataNodeInfo>> list_datanodes_by_state(DataNodeState state) override;
    pl::Result<std::vector<DataNodeInfo>> list_all_datanodes() override;
    pl::Result<pl::Void> upsert_datanode(const DataNodeInfo& info) override;

    // Lease operations

    pl::Result<pl::Void> create_lease(const Lease& lease) override;
    pl::Result<std::optional<Lease>> get_active_lease(uint64_t inode_id) override;
    pl::Result<pl::Void> renew_lease(uint64_t inode_id, uint64_t new_expire_ms) override;
    pl::Result<pl::Void> close_lease(uint64_t inode_id) override;
    pl::Result<uint64_t> expire_leases(uint64_t now_ms) override;
    pl::Result<std::vector<Lease>> list_expired_leases(uint64_t now_ms) override;

    // ID Allocation

    pl::Result<uint64_t> alloc_id(std::string_view name, uint64_t count = 1) override;

    // Operation Log

    pl::Result<pl::Void> write_oplog(std::string_view op_type,
                                     uint64_t target_inode_id,
                                     std::string_view request_id,
                                     std::string_view payload_json) override;
    pl::Result<bool> check_request_id(std::string_view request_id) override;
    pl::Result<std::optional<OplogEntry>> get_oplog_by_request_id(
        std::string_view request_id) override;

    /// Bind a connection to the current thread (used by transactions).
    /// While bound, all store operations use this connection instead of acquiring from pool.
    void bind_connection(PooledConnection* conn);

    /// Unbind the thread-local connection.
    void unbind_connection();

private:
    explicit MySQLMetadataStore(std::shared_ptr<MySQLConnectionPool> pool);

    /// Acquire a fresh connection from pool (when not in a transaction).
    pl::Result<PooledConnection> acquire_connection();

    /// Get the connection to use: bound (in-transaction) or the owned one.
    PooledConnection* get_active_conn(PooledConnection& owned);

    std::shared_ptr<MySQLConnectionPool> pool_;
    static thread_local PooledConnection* bound_conn_;
};

} // namespace pl::minidfs
