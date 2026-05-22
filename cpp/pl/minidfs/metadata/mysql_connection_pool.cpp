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

#include "cpp/pl/minidfs/metadata/mysql_connection_pool.h"

#include <boost/mysql/error_with_diagnostics.hpp>

#include "cpp/pl/minidfs/common/error_code.h"

namespace pl::minidfs {

// ============================================================================
// PooledConnection implementation
// ============================================================================

PooledConnection::PooledConnection(std::unique_ptr<boost::mysql::any_connection> conn,
                                   std::shared_ptr<boost::asio::io_context> io_ctx,
                                   MySQLConnectionPool* pool)
    : conn_(std::move(conn)), io_ctx_(std::move(io_ctx)), pool_(pool) {}

PooledConnection::~PooledConnection() {
    if (conn_ && pool_) {
        pool_->release(std::move(conn_), std::move(io_ctx_));
    }
}

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : conn_(std::move(other.conn_)), io_ctx_(std::move(other.io_ctx_)), pool_(other.pool_) {
    other.pool_ = nullptr;
}

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        if (conn_ && pool_) {
            pool_->release(std::move(conn_), std::move(io_ctx_));
        }
        conn_ = std::move(other.conn_);
        io_ctx_ = std::move(other.io_ctx_);
        pool_ = other.pool_;
        other.pool_ = nullptr;
    }
    return *this;
}

pl::Result<boost::mysql::results> PooledConnection::execute(std::string_view sql) {
    try {
        boost::mysql::results result;
        conn_->execute(sql, result);
        return result;
    } catch (const boost::mysql::error_with_diagnostics& e) {
        return folly::makeUnexpected(
            pl::Status(static_cast<pl::status_code_t>(ErrorCode::kMySQLQueryFailed), e.what()));
    }
}

// ============================================================================
// MySQLConnectionPool implementation
// ============================================================================

MySQLConnectionPool::MySQLConnectionPool(const MySQLConfig& config)
    : config_(config), pool_size_(config.pool_size) {}

MySQLConnectionPool::~MySQLConnectionPool() {
    std::lock_guard lock(mutex_);
    while (!idle_.empty()) {
        auto& entry = idle_.front();
        try {
            entry.conn->close();
        } catch (...) {
            // best-effort close
        }
        idle_.pop();
    }
}

pl::Result<std::shared_ptr<MySQLConnectionPool>> MySQLConnectionPool::create(
    const MySQLConfig& config) {
    // Use raw new because constructor is private
    auto pool = std::shared_ptr<MySQLConnectionPool>(new MySQLConnectionPool(config));
    auto res = pool->init();
    if (res.hasError()) {
        return folly::makeUnexpected(res.error());
    }
    return pool;
}

pl::Result<pl::Void> MySQLConnectionPool::init() {
    for (uint32_t i = 0; i < pool_size_; ++i) {
        auto conn_result = create_connection();
        if (conn_result.hasError()) {
            return folly::makeUnexpected(conn_result.error());
        }
        auto& [conn, io_ctx] = conn_result.value();
        idle_.push(PoolEntry{std::move(conn), std::move(io_ctx)});
    }
    return pl::Void{};
}

pl::Result<std::pair<std::unique_ptr<boost::mysql::any_connection>,
                     std::shared_ptr<boost::asio::io_context>>>
MySQLConnectionPool::create_connection() {
    try {
        auto io_ctx = std::make_shared<boost::asio::io_context>();
        auto conn = std::make_unique<boost::mysql::any_connection>(*io_ctx);

        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(config_.host, config_.port);
        params.username = config_.user;
        params.password = config_.password;
        params.database = config_.database;

        conn->connect(params);
        return std::make_pair(std::move(conn), std::move(io_ctx));
    } catch (const boost::mysql::error_with_diagnostics& e) {
        return folly::makeUnexpected(
            pl::Status(static_cast<pl::status_code_t>(ErrorCode::kMySQLConnectFailed),
                       std::string("MySQL connection failed: ") + e.what()));
    }
}

pl::Result<PooledConnection> MySQLConnectionPool::acquire() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !idle_.empty(); });

    auto entry = std::move(idle_.front());
    idle_.pop();
    return PooledConnection(std::move(entry.conn), std::move(entry.io_ctx), this);
}

void MySQLConnectionPool::release(std::unique_ptr<boost::mysql::any_connection> conn,
                                  std::shared_ptr<boost::asio::io_context> io_ctx) {
    {
        std::lock_guard lock(mutex_);
        idle_.push(PoolEntry{std::move(conn), std::move(io_ctx)});
    }
    cv_.notify_one();
}

} // namespace pl::minidfs
