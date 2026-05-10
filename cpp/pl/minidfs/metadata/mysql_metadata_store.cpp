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

#include "cpp/pl/minidfs/metadata/mysql_metadata_store.h"

#include "cpp/pl/minidfs/common/error_code.h"
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/row_view.hpp>
#include <boost/mysql/string_view.hpp>
#include <fmt/format.h>

namespace pl::minidfs {

// ============================================================================
// MySQLTransaction -- RAII transaction using a pooled connection.
// ============================================================================

class MySQLTransaction final : public Transaction {
public:
    explicit MySQLTransaction(PooledConnection conn) : conn_(std::move(conn)) {}

    ~MySQLTransaction() override {
        if (!committed_) {
            rollback();
        }
    }

    pl::Result<pl::Void> commit() override {
        if (committed_) {
            return pl::Void{};
        }
        auto res = conn_.execute("COMMIT");
        if (res.hasError()) {
            return folly::makeUnexpected(res.error());
        }
        committed_ = true;
        return pl::Void{};
    }

    void rollback() override {
        if (!committed_) {
            (void)conn_.execute("ROLLBACK");
            committed_ = true; // prevent double rollback
        }
    }

private:
    PooledConnection conn_;
    bool committed_ = false;
};

// ============================================================================
// Helper: row field extraction
// ============================================================================

namespace {

inline uint64_t to_u64(const boost::mysql::field_view& fv) {
    if (fv.is_uint64()) {
        return fv.as_uint64();
    }
    return static_cast<uint64_t>(fv.as_int64());
}

inline uint32_t to_u32(const boost::mysql::field_view& fv) {
    if (fv.is_uint64()) {
        return static_cast<uint32_t>(fv.as_uint64());
    }
    return static_cast<uint32_t>(fv.as_int64());
}

inline std::string to_str(const boost::mysql::field_view& fv) {
    auto sv = fv.as_string();
    return std::string(sv.data(), sv.size());
}

Inode row_to_inode(const boost::mysql::row_view& row) {
    Inode inode;
    inode.inode_id = to_u64(row[0]);
    inode.type = static_cast<InodeType>(to_u32(row[1]));
    inode.parent_id = to_u64(row[2]);
    inode.name = to_str(row[3]);
    inode.owner = to_str(row[4]);
    inode.group = to_str(row[5]);
    inode.permission = to_u32(row[6]);
    inode.length = to_u64(row[7]);
    inode.replication = to_u32(row[8]);
    inode.block_size = to_u64(row[9]);
    inode.state = static_cast<FileState>(to_u32(row[10]));
    inode.ctime_ms = to_u64(row[11]);
    inode.mtime_ms = to_u64(row[12]);
    inode.version = to_u64(row[13]);
    return inode;
}

BlockMeta row_to_block(const boost::mysql::row_view& row) {
    BlockMeta blk;
    blk.block_id = to_u64(row[0]);
    blk.inode_id = to_u64(row[1]);
    blk.block_index = to_u32(row[2]);
    blk.generation_stamp = to_u64(row[3]);
    blk.length = to_u64(row[4]);
    blk.state = static_cast<BlockState>(to_u32(row[5]));
    blk.desired_replica = to_u32(row[6]);
    blk.ctime_ms = to_u64(row[7]);
    blk.mtime_ms = to_u64(row[8]);
    return blk;
}

BlockReplica row_to_replica(const boost::mysql::row_view& row) {
    BlockReplica r;
    r.block_id = to_u64(row[0]);
    r.datanode_id = to_u64(row[1]);
    r.storage_id = to_u64(row[2]);
    r.state = static_cast<ReplicaState>(to_u32(row[3]));
    r.length = to_u64(row[4]);
    r.generation_stamp = to_u64(row[5]);
    r.report_time_ms = to_u64(row[6]);
    return r;
}

DataNodeInfo row_to_datanode(const boost::mysql::row_view& row) {
    DataNodeInfo dn;
    dn.datanode_id = to_u64(row[0]);
    dn.uuid = to_str(row[1]);
    dn.hostname = to_str(row[2]);
    dn.ip = to_str(row[3]);
    dn.rpc_port = to_u32(row[4]);
    dn.data_port = to_u32(row[5]);
    dn.rack = to_str(row[6]);
    dn.state = static_cast<DataNodeState>(to_u32(row[7]));
    dn.capacity_bytes = to_u64(row[8]);
    dn.used_bytes = to_u64(row[9]);
    dn.free_bytes = to_u64(row[10]);
    dn.last_heartbeat_ms = to_u64(row[11]);
    return dn;
}

Lease row_to_lease(const boost::mysql::row_view& row) {
    Lease l;
    l.lease_id = to_u64(row[0]);
    l.inode_id = to_u64(row[1]);
    l.client_id = to_str(row[2]);
    l.state = static_cast<LeaseState>(to_u32(row[3]));
    l.expire_time_ms = to_u64(row[4]);
    l.ctime_ms = to_u64(row[5]);
    l.mtime_ms = to_u64(row[6]);
    return l;
}

} // namespace

// ============================================================================
// MySQLMetadataStore construction
// ============================================================================

MySQLMetadataStore::MySQLMetadataStore(std::shared_ptr<MySQLConnectionPool> pool)
    : pool_(std::move(pool)) {}

pl::Result<std::unique_ptr<MySQLMetadataStore>> MySQLMetadataStore::create(
    std::shared_ptr<MySQLConnectionPool> pool) {
    if (!pool) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInvalidArgument),
                             "pool must not be null");
    }
    return std::unique_ptr<MySQLMetadataStore>(new MySQLMetadataStore(std::move(pool)));
}

// ============================================================================
// Transaction management
// ============================================================================

pl::Result<std::unique_ptr<Transaction>> MySQLMetadataStore::begin_transaction() {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto res = conn.execute("BEGIN");
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return std::unique_ptr<Transaction>(std::make_unique<MySQLTransaction>(std::move(conn)));
}

// ============================================================================
// Inode operations
// ============================================================================

pl::Result<Inode> MySQLMetadataStore::get_inode(uint64_t inode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
                           "length, replication, block_size, state, ctime_ms, mtime_ms, version "
                           "FROM inodes WHERE inode_id = {}",
                           inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                             fmt::format("inode {} not found", inode_id));
    }
    return row_to_inode(rows[0]);
}

pl::Result<std::optional<Inode>> MySQLMetadataStore::get_child(uint64_t parent_id,
                                                               std::string_view name) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
                           "length, replication, block_size, state, ctime_ms, mtime_ms, version "
                           "FROM inodes WHERE parent_id = {} AND name = '{}'",
                           parent_id, name);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return std::optional<Inode>(std::nullopt);
    }
    return std::optional<Inode>(row_to_inode(rows[0]));
}

pl::Result<std::vector<Inode>> MySQLMetadataStore::list_children(uint64_t parent_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
                           "length, replication, block_size, state, ctime_ms, mtime_ms, version "
                           "FROM inodes WHERE parent_id = {} ORDER BY name",
                           parent_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<Inode> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_inode(row));
    }
    return result;
}

pl::Result<pl::Void> MySQLMetadataStore::create_inode(const Inode& inode) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format(
        "INSERT INTO inodes (inode_id, type, parent_id, name, owner, `group`, "
        "permission, length, replication, block_size, state, ctime_ms, mtime_ms, version) "
        "VALUES ({}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {}, {}, {}, {}, {})",
        inode.inode_id, static_cast<uint8_t>(inode.type), inode.parent_id, inode.name, inode.owner,
        inode.group, inode.permission, inode.length, inode.replication, inode.block_size,
        static_cast<uint8_t>(inode.state), inode.ctime_ms, inode.mtime_ms, inode.version);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_inode(const Inode& inode) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format(
        "UPDATE inodes SET type={}, parent_id={}, name='{}', owner='{}', `group`='{}', "
        "permission={}, length={}, replication={}, block_size={}, state={}, "
        "mtime_ms={}, version=version+1 WHERE inode_id={}",
        static_cast<uint8_t>(inode.type), inode.parent_id, inode.name, inode.owner, inode.group,
        inode.permission, inode.length, inode.replication, inode.block_size,
        static_cast<uint8_t>(inode.state), inode.mtime_ms, inode.inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::delete_inode(uint64_t inode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("DELETE FROM inodes WHERE inode_id = {}", inode_id);
    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// ============================================================================
// Block operations
// ============================================================================

pl::Result<BlockMeta> MySQLMetadataStore::get_block(uint64_t block_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE block_id = {}",
                           block_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                             fmt::format("block {} not found", block_id));
    }
    return row_to_block(rows[0]);
}

pl::Result<std::vector<BlockMeta>> MySQLMetadataStore::get_blocks_by_inode(uint64_t inode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE inode_id = {} ORDER BY block_index",
                           inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<BlockMeta> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_block(row));
    }
    return result;
}

pl::Result<pl::Void> MySQLMetadataStore::create_block(const BlockMeta& block) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("INSERT INTO blocks (block_id, inode_id, block_index, generation_stamp, "
                           "length, state, desired_replica, ctime_ms, mtime_ms) "
                           "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {})",
                           block.block_id, block.inode_id, block.block_index,
                           block.generation_stamp, block.length, static_cast<uint8_t>(block.state),
                           block.desired_replica, block.ctime_ms, block.mtime_ms);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_block(const BlockMeta& block) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("UPDATE blocks SET inode_id={}, block_index={}, generation_stamp={}, "
                           "length={}, state={}, desired_replica={}, mtime_ms={} WHERE block_id={}",
                           block.inode_id, block.block_index, block.generation_stamp, block.length,
                           static_cast<uint8_t>(block.state), block.desired_replica, block.mtime_ms,
                           block.block_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<std::vector<BlockMeta>> MySQLMetadataStore::get_blocks_by_state(BlockState state) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE state = {}",
                           static_cast<uint8_t>(state));

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<BlockMeta> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_block(row));
    }
    return result;
}

// ============================================================================
// Block Replica operations
// ============================================================================

pl::Result<std::vector<BlockReplica>> MySQLMetadataStore::get_replicas(uint64_t block_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT block_id, datanode_id, storage_id, state, length, "
                           "generation_stamp, report_time_ms "
                           "FROM block_replicas WHERE block_id = {}",
                           block_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<BlockReplica> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_replica(row));
    }
    return result;
}

pl::Result<std::vector<BlockReplica>> MySQLMetadataStore::get_replicas_by_datanode(
    uint64_t datanode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT block_id, datanode_id, storage_id, state, length, "
                           "generation_stamp, report_time_ms "
                           "FROM block_replicas WHERE datanode_id = {}",
                           datanode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<BlockReplica> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_replica(row));
    }
    return result;
}

pl::Result<pl::Void> MySQLMetadataStore::upsert_replica(const BlockReplica& replica) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format(
        "INSERT INTO block_replicas (block_id, datanode_id, storage_id, state, "
        "length, generation_stamp, report_time_ms) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE state={}, length={}, generation_stamp={}, report_time_ms={}",
        replica.block_id, replica.datanode_id, replica.storage_id,
        static_cast<uint8_t>(replica.state), replica.length, replica.generation_stamp,
        replica.report_time_ms, static_cast<uint8_t>(replica.state), replica.length,
        replica.generation_stamp, replica.report_time_ms);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::delete_replicas_by_block(uint64_t block_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("DELETE FROM block_replicas WHERE block_id = {}", block_id);
    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_replica_state(uint64_t block_id,
                                                              uint64_t datanode_id,
                                                              ReplicaState new_state) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql =
        fmt::format("UPDATE block_replicas SET state={} WHERE block_id={} AND datanode_id={}",
                    static_cast<uint8_t>(new_state), block_id, datanode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// ============================================================================
// DataNode operations
// ============================================================================

pl::Result<DataNodeInfo> MySQLMetadataStore::get_datanode(uint64_t datanode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE datanode_id = {}",
                           datanode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kNotFound),
                             fmt::format("datanode {} not found", datanode_id));
    }
    return row_to_datanode(rows[0]);
}

pl::Result<std::optional<DataNodeInfo>> MySQLMetadataStore::get_datanode_by_uuid(
    std::string_view uuid) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE uuid = '{}'",
                           uuid);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return std::optional<DataNodeInfo>(std::nullopt);
    }
    return std::optional<DataNodeInfo>(row_to_datanode(rows[0]));
}

pl::Result<std::vector<DataNodeInfo>> MySQLMetadataStore::list_datanodes_by_state(
    DataNodeState state) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE state = {}",
                           static_cast<uint8_t>(state));

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<DataNodeInfo> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_datanode(row));
    }
    return result;
}

pl::Result<std::vector<DataNodeInfo>> MySQLMetadataStore::list_all_datanodes() {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto res = conn.execute("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                            "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                            "FROM datanodes");
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<DataNodeInfo> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_datanode(row));
    }
    return result;
}

pl::Result<pl::Void> MySQLMetadataStore::upsert_datanode(const DataNodeInfo& info) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format(
        "INSERT INTO datanodes (datanode_id, uuid, hostname, ip, rpc_port, data_port, "
        "rack, state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms) "
        "VALUES ({}, '{}', '{}', '{}', {}, {}, '{}', {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE hostname='{}', ip='{}', rpc_port={}, data_port={}, "
        "rack='{}', state={}, capacity_bytes={}, used_bytes={}, free_bytes={}, "
        "last_heartbeat_ms={}",
        info.datanode_id, info.uuid, info.hostname, info.ip, info.rpc_port, info.data_port,
        info.rack, static_cast<uint8_t>(info.state), info.capacity_bytes, info.used_bytes,
        info.free_bytes, info.last_heartbeat_ms, info.hostname, info.ip, info.rpc_port,
        info.data_port, info.rack, static_cast<uint8_t>(info.state), info.capacity_bytes,
        info.used_bytes, info.free_bytes, info.last_heartbeat_ms);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// ============================================================================
// Lease operations
// ============================================================================

pl::Result<pl::Void> MySQLMetadataStore::create_lease(const Lease& lease) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("INSERT INTO leases (lease_id, inode_id, client_id, state, "
                           "expire_time_ms, ctime_ms, mtime_ms) "
                           "VALUES ({}, {}, '{}', {}, {}, {}, {})",
                           lease.lease_id, lease.inode_id, lease.client_id,
                           static_cast<uint8_t>(lease.state), lease.expire_time_ms, lease.ctime_ms,
                           lease.mtime_ms);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<std::optional<Lease>> MySQLMetadataStore::get_active_lease(uint64_t inode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format(
        "SELECT lease_id, inode_id, client_id, state, expire_time_ms, ctime_ms, mtime_ms "
        "FROM leases WHERE inode_id = {} AND state = 0 LIMIT 1",
        inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return std::optional<Lease>(std::nullopt);
    }
    return std::optional<Lease>(row_to_lease(rows[0]));
}

pl::Result<pl::Void> MySQLMetadataStore::renew_lease(uint64_t inode_id, uint64_t new_expire_ms) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql =
        fmt::format("UPDATE leases SET expire_time_ms = {} WHERE inode_id = {} AND state = 0",
                    new_expire_ms, inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::close_lease(uint64_t inode_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql =
        fmt::format("UPDATE leases SET state = 1 WHERE inode_id = {} AND state = 0", inode_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<uint64_t> MySQLMetadataStore::expire_leases(uint64_t now_ms) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql =
        fmt::format("UPDATE leases SET state = 1 WHERE state = 0 AND expire_time_ms < {}", now_ms);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return res.value().affected_rows();
}

// ============================================================================
// ID Allocation
// ============================================================================

pl::Result<uint64_t> MySQLMetadataStore::alloc_id(std::string_view name, uint64_t count) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    // Atomic allocation via INSERT ... ON DUPLICATE KEY UPDATE.
    // Table: id_allocators(name VARCHAR PK, next_id BIGINT UNSIGNED).
    auto sql = fmt::format("INSERT INTO id_allocators (name, next_id) VALUES ('{}', {}) "
                           "ON DUPLICATE KEY UPDATE next_id = next_id + {}",
                           name, count, count);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    // Read back the current value to determine the allocated range.
    auto read_sql = fmt::format("SELECT next_id FROM id_allocators WHERE name = '{}'", name);
    auto read_res = conn.execute(read_sql);
    if (read_res.hasError()) {
        return folly::makeUnexpected(read_res.error());
    }

    auto rows = read_res.value().rows();
    if (rows.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInternalError),
                             "id_allocators: read after write failed");
    }

    uint64_t next_id = to_u64(rows[0][0]);
    // Allocated range: [next_id - count, next_id). Return first ID.
    return next_id - count;
}

// ============================================================================
// Operation Log
// ============================================================================

pl::Result<pl::Void> MySQLMetadataStore::write_oplog(std::string_view op_type,
                                                     uint64_t target_inode_id,
                                                     std::string_view request_id,
                                                     std::string_view payload_json) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql =
        fmt::format("INSERT INTO op_log (op_type, target_inode_id, request_id, payload_json) "
                    "VALUES ('{}', {}, '{}', '{}')",
                    op_type, target_inode_id, request_id, payload_json);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<bool> MySQLMetadataStore::check_request_id(std::string_view request_id) {
    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());

    auto sql = fmt::format("SELECT 1 FROM op_log WHERE request_id = '{}' LIMIT 1", request_id);

    auto res = conn.execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return !res.value().rows().empty();
}

} // namespace pl::minidfs
