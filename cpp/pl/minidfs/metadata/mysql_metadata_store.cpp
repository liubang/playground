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

#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/row_view.hpp>
#include <boost/mysql/string_view.hpp>
#include <fmt/format.h>

#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

// MySQLTransaction -- RAII transaction using a pooled connection.

class MySQLTransaction final : public Transaction {
public:
    MySQLTransaction(PooledConnection conn, MySQLMetadataStore* store)
        : conn_(std::move(conn)), store_(store) {
        store_->bind_connection(&conn_);
    }

    ~MySQLTransaction() override {
        if (!committed_) {
            rollback();
        }
        store_->unbind_connection(&conn_);
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
    MySQLMetadataStore* store_;
    bool committed_ = false;
};

// Helper: row field extraction

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
    inode.file_append_mode = static_cast<FileAppendMode>(to_u32(row[10]));
    inode.content_generation = to_u64(row[11]);
    inode.checksum = to_u32(row[12]);
    inode.checksum_valid = to_u32(row[13]) != 0;
    inode.state = static_cast<FileState>(to_u32(row[14]));
    inode.ctime_ms = to_u64(row[15]);
    inode.mtime_ms = to_u64(row[16]);
    inode.version = to_u64(row[17]);
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

// Escape special characters in SQL string literals to prevent injection.
inline std::string escape_sql(std::string_view input) {
    std::string result;
    result.reserve(input.size() + input.size() / 8);
    for (char c : input) {
        switch (c) {
            case '\'':
                result.append("\\'");
                break;
            case '\\':
                result.append("\\\\");
                break;
            case '"':
                result.append("\\\"");
                break;
            case '\0':
                result.append("\\0");
                break;
            case '\n':
                result.append("\\n");
                break;
            case '\r':
                result.append("\\r");
                break;
            case '\x1a':
                result.append("\\Z");
                break;
            default:
                result.push_back(c);
                break;
        }
    }
    return result;
}

} // namespace

// MySQLMetadataStore construction

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

// Transaction management

thread_local MySQLMetadataStore::ConnectionBinding MySQLMetadataStore::binding_{};

bool MySQLMetadataStore::has_active_transaction() const {
    return binding_.store == this && binding_.connection != nullptr;
}

PooledConnection* MySQLMetadataStore::active_bound_connection() {
    return has_active_transaction() ? binding_.connection : nullptr;
}

void MySQLMetadataStore::bind_connection(PooledConnection* conn) {
    binding_ = ConnectionBinding{.store = this, .connection = conn};
}

void MySQLMetadataStore::unbind_connection(PooledConnection* conn) {
    if (binding_.store == this && binding_.connection == conn) {
        binding_ = {};
    }
}

pl::Result<PooledConnection> MySQLMetadataStore::acquire_connection() {
    return pool_->acquire();
}

PooledConnection* MySQLMetadataStore::get_active_conn(PooledConnection& owned) {
    auto* bound = active_bound_connection();
    return bound != nullptr ? bound : &owned;
}

pl::Result<std::unique_ptr<Transaction>> MySQLMetadataStore::begin_transaction() {
    if (binding_.connection != nullptr) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kMySQLTxnFailed),
                             "a transaction is already active on this thread");
    }

    auto conn_result = pool_->acquire();
    if (conn_result.hasError()) {
        return folly::makeUnexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto res = conn.execute("BEGIN");
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return std::unique_ptr<Transaction>(std::make_unique<MySQLTransaction>(std::move(conn), this));
}

// Inode operations

pl::Result<Inode> MySQLMetadataStore::get_inode(uint64_t inode_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
        "length, replication, block_size, file_append_mode, content_generation, checksum, "
        "checksum_valid, state, ctime_ms, mtime_ms, version "
        "FROM inodes WHERE inode_id = {}",
        inode_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
        "length, replication, block_size, file_append_mode, content_generation, checksum, "
        "checksum_valid, state, ctime_ms, mtime_ms, version "
        "FROM inodes WHERE parent_id = {} AND name = '{}'",
        parent_id,
        escape_sql(name));

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "SELECT inode_id, type, parent_id, name, owner, `group`, permission, "
        "length, replication, block_size, file_append_mode, content_generation, checksum, "
        "checksum_valid, state, ctime_ms, mtime_ms, version "
        "FROM inodes WHERE parent_id = {} ORDER BY name",
        parent_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "INSERT INTO inodes (inode_id, type, parent_id, name, owner, `group`, "
        "permission, length, replication, block_size, file_append_mode, content_generation, "
        "checksum, checksum_valid, state, ctime_ms, mtime_ms, version) "
        "VALUES ({}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        inode.inode_id,
        static_cast<uint8_t>(inode.type),
        inode.parent_id,
        escape_sql(inode.name),
        escape_sql(inode.owner),
        escape_sql(inode.group),
        inode.permission,
        inode.length,
        inode.replication,
        inode.block_size,
        static_cast<uint8_t>(inode.file_append_mode),
        inode.content_generation,
        inode.checksum,
        inode.checksum_valid,
        static_cast<uint8_t>(inode.state),
        inode.ctime_ms,
        inode.mtime_ms,
        inode.version);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_inode(const Inode& inode) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("UPDATE inodes SET type={}, parent_id={}, name='{}', owner='{}', `group`='{}', "
                    "permission={}, length={}, replication={}, block_size={}, file_append_mode={}, "
                    "content_generation={}, checksum={}, checksum_valid={}, state={}, "
                    "mtime_ms={}, version=version+1 WHERE inode_id={} AND version={}",
                    static_cast<uint8_t>(inode.type),
                    inode.parent_id,
                    escape_sql(inode.name),
                    escape_sql(inode.owner),
                    escape_sql(inode.group),
                    inode.permission,
                    inode.length,
                    inode.replication,
                    inode.block_size,
                    static_cast<uint8_t>(inode.file_append_mode),
                    inode.content_generation,
                    inode.checksum,
                    inode.checksum_valid,
                    static_cast<uint8_t>(inode.state),
                    inode.mtime_ms,
                    inode.inode_id,
                    inode.version);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    // CAS check: if no rows updated, either inode not found or version conflict.
    if (res.value().affected_rows() == 0) {
        return pl::makeError(
            static_cast<pl::status_code_t>(ErrorCode::kInternalError),
            fmt::format("inode {} update failed: not found or version conflict (version={})",
                        inode.inode_id,
                        inode.version));
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::delete_inode(uint64_t inode_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("DELETE FROM inodes WHERE inode_id = {}", inode_id);
    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// Block operations

pl::Result<BlockMeta> MySQLMetadataStore::get_block(uint64_t block_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE block_id = {}",
                           block_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE inode_id = {} ORDER BY block_index",
                           inode_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("INSERT INTO blocks (block_id, inode_id, block_index, generation_stamp, "
                           "length, state, desired_replica, ctime_ms, mtime_ms) "
                           "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {})",
                           block.block_id,
                           block.inode_id,
                           block.block_index,
                           block.generation_stamp,
                           block.length,
                           static_cast<uint8_t>(block.state),
                           block.desired_replica,
                           block.ctime_ms,
                           block.mtime_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::delete_block(uint64_t block_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("DELETE FROM blocks WHERE block_id = {}", block_id);
    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_block(const BlockMeta& block) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("UPDATE blocks SET inode_id={}, block_index={}, generation_stamp={}, "
                           "length={}, state={}, desired_replica={}, mtime_ms={} WHERE block_id={}",
                           block.inode_id,
                           block.block_index,
                           block.generation_stamp,
                           block.length,
                           static_cast<uint8_t>(block.state),
                           block.desired_replica,
                           block.mtime_ms,
                           block.block_id);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<std::vector<BlockMeta>> MySQLMetadataStore::get_blocks_by_state(BlockState state) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT block_id, inode_id, block_index, generation_stamp, length, "
                           "state, desired_replica, ctime_ms, mtime_ms "
                           "FROM blocks WHERE state = {}",
                           static_cast<uint8_t>(state));

    auto res = conn->execute(sql);
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

// Block Replica operations

pl::Result<std::vector<BlockReplica>> MySQLMetadataStore::get_replicas(uint64_t block_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT block_id, datanode_id, storage_id, state, length, "
                           "generation_stamp, report_time_ms "
                           "FROM block_replicas WHERE block_id = {}",
                           block_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT block_id, datanode_id, storage_id, state, length, "
                           "generation_stamp, report_time_ms "
                           "FROM block_replicas WHERE datanode_id = {}",
                           datanode_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "INSERT INTO block_replicas (block_id, datanode_id, storage_id, state, "
        "length, generation_stamp, report_time_ms) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE state={}, length={}, generation_stamp={}, report_time_ms={}",
        replica.block_id,
        replica.datanode_id,
        replica.storage_id,
        static_cast<uint8_t>(replica.state),
        replica.length,
        replica.generation_stamp,
        replica.report_time_ms,
        static_cast<uint8_t>(replica.state),
        replica.length,
        replica.generation_stamp,
        replica.report_time_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::delete_replicas_by_block(uint64_t block_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("DELETE FROM block_replicas WHERE block_id = {}", block_id);
    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::update_replica_state(uint64_t block_id,
                                                              uint64_t datanode_id,
                                                              ReplicaState new_state) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("UPDATE block_replicas SET state={} WHERE block_id={} AND datanode_id={}",
                    static_cast<uint8_t>(new_state),
                    block_id,
                    datanode_id);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// DataNode operations

pl::Result<DataNodeInfo> MySQLMetadataStore::get_datanode(uint64_t datanode_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE datanode_id = {}",
                           datanode_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE uuid = '{}'",
                           escape_sql(uuid));

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
                           "state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms "
                           "FROM datanodes WHERE state = {}",
                           static_cast<uint8_t>(state));

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto res = conn->execute("SELECT datanode_id, uuid, hostname, ip, rpc_port, data_port, rack, "
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("INSERT INTO datanodes (datanode_id, uuid, hostname, ip, rpc_port, data_port, "
                    "rack, state, capacity_bytes, used_bytes, free_bytes, last_heartbeat_ms) "
                    "VALUES ({}, '{}', '{}', '{}', {}, {}, '{}', {}, {}, {}, {}, {}) "
                    "ON DUPLICATE KEY UPDATE hostname='{}', ip='{}', rpc_port={}, data_port={}, "
                    "rack='{}', state={}, capacity_bytes={}, used_bytes={}, free_bytes={}, "
                    "last_heartbeat_ms={}",
                    info.datanode_id,
                    escape_sql(info.uuid),
                    escape_sql(info.hostname),
                    escape_sql(info.ip),
                    info.rpc_port,
                    info.data_port,
                    escape_sql(info.rack),
                    static_cast<uint8_t>(info.state),
                    info.capacity_bytes,
                    info.used_bytes,
                    info.free_bytes,
                    info.last_heartbeat_ms,
                    escape_sql(info.hostname),
                    escape_sql(info.ip),
                    info.rpc_port,
                    info.data_port,
                    escape_sql(info.rack),
                    static_cast<uint8_t>(info.state),
                    info.capacity_bytes,
                    info.used_bytes,
                    info.free_bytes,
                    info.last_heartbeat_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

// Lease operations

pl::Result<pl::Void> MySQLMetadataStore::create_lease(const Lease& lease) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("INSERT INTO leases (lease_id, inode_id, client_id, state, "
                           "expire_time_ms, ctime_ms, mtime_ms) "
                           "VALUES ({}, {}, '{}', {}, {}, {}, {})",
                           lease.lease_id,
                           lease.inode_id,
                           escape_sql(lease.client_id),
                           static_cast<uint8_t>(lease.state),
                           lease.expire_time_ms,
                           lease.ctime_ms,
                           lease.mtime_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<std::optional<Lease>> MySQLMetadataStore::get_active_lease(uint64_t inode_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "SELECT lease_id, inode_id, client_id, state, expire_time_ms, ctime_ms, mtime_ms "
        "FROM leases WHERE inode_id = {} AND state = 0 LIMIT 1",
        inode_id);

    auto res = conn->execute(sql);
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
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("UPDATE leases SET expire_time_ms = {} WHERE inode_id = {} AND state = 0",
                    new_expire_ms,
                    inode_id);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<pl::Void> MySQLMetadataStore::close_lease(uint64_t inode_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("UPDATE leases SET state = 1 WHERE inode_id = {} AND state = 0", inode_id);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<uint64_t> MySQLMetadataStore::expire_leases(uint64_t now_ms) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("UPDATE leases SET state = 1 WHERE state = 0 AND expire_time_ms < {}", now_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return res.value().affected_rows();
}

pl::Result<std::vector<Lease>> MySQLMetadataStore::list_expired_leases(uint64_t now_ms) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format(
        "SELECT lease_id, inode_id, client_id, state, expire_time_ms, ctime_ms, mtime_ms "
        "FROM leases WHERE state = 0 AND expire_time_ms < {}",
        now_ms);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    std::vector<Lease> result;
    auto rows = res.value().rows();
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back(row_to_lease(row));
    }
    return result;
}

// ID Allocation

pl::Result<uint64_t> MySQLMetadataStore::alloc_id(std::string_view name, uint64_t count) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    // Atomic allocation via INSERT ... ON DUPLICATE KEY UPDATE with
    // LAST_INSERT_ID() trick to avoid TOCTOU between UPDATE and SELECT.
    // Table: id_allocators(name VARCHAR PK, next_id BIGINT UNSIGNED).
    //
    // INSERT path: LAST_INSERT_ID(0) sets the session value to 0 (the base of
    // the first allocated range), and next_id becomes 0 + count.
    // UPDATE path: LAST_INSERT_ID(next_id) captures the current next_id as base,
    // then adds count.
    auto sql = fmt::format(
        "INSERT INTO id_allocators (name, next_id) VALUES ('{}', LAST_INSERT_ID(0) + {}) "
        "ON DUPLICATE KEY UPDATE next_id = LAST_INSERT_ID(next_id) + {}",
        escape_sql(name),
        count,
        count);

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    // LAST_INSERT_ID() returns the session-local value set by the UPDATE above.
    // This is immune to concurrent modifications on other connections.
    auto read_res = conn->execute("SELECT LAST_INSERT_ID()");
    if (read_res.hasError()) {
        return folly::makeUnexpected(read_res.error());
    }

    auto rows = read_res.value().rows();
    if (rows.empty()) {
        return pl::makeError(static_cast<pl::status_code_t>(ErrorCode::kInternalError),
                             "id_allocators: LAST_INSERT_ID() returned no rows");
    }

    // LAST_INSERT_ID() gives us the value of next_id BEFORE the addition.
    // So the allocated range is [last_insert_id, last_insert_id + count).
    uint64_t base_id = to_u64(rows[0][0]);
    return base_id;
}

// Operation Log

pl::Result<pl::Void> MySQLMetadataStore::write_oplog(std::string_view op_type,
                                                     uint64_t target_inode_id,
                                                     std::string_view request_id,
                                                     std::string_view payload_json) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("INSERT INTO op_log (op_type, target_inode_id, request_id, payload_json) "
                    "VALUES ('{}', {}, '{}', '{}')",
                    escape_sql(op_type),
                    target_inode_id,
                    escape_sql(request_id),
                    escape_sql(payload_json));

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pl::Void{};
}

pl::Result<std::optional<OplogEntry>> MySQLMetadataStore::get_oplog_by_request_id(
    std::string_view request_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql = fmt::format("SELECT op_type, target_inode_id, request_id, payload_json "
                           "FROM op_log WHERE request_id = '{}' LIMIT 1",
                           escape_sql(request_id));

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }

    auto rows = res.value().rows();
    if (rows.empty()) {
        return std::optional<OplogEntry>(std::nullopt);
    }

    OplogEntry entry;
    entry.op_type = to_str(rows[0][0]);
    entry.target_inode_id = to_u64(rows[0][1]);
    entry.request_id = to_str(rows[0][2]);
    entry.payload_json = to_str(rows[0][3]);
    return std::optional<OplogEntry>(std::move(entry));
}

pl::Result<bool> MySQLMetadataStore::check_request_id(std::string_view request_id) {
    PooledConnection owned;
    if (active_bound_connection() == nullptr) {
        auto conn_result = pool_->acquire();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        owned = std::move(conn_result.value());
    }
    auto* conn = get_active_conn(owned);

    auto sql =
        fmt::format("SELECT 1 FROM op_log WHERE request_id = '{}' LIMIT 1", escape_sql(request_id));

    auto res = conn->execute(sql);
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return !res.value().rows().empty();
}

} // namespace pl::minidfs
