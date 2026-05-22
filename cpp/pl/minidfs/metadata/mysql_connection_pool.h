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

#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/results.hpp>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include "cpp/pl/status/result.h"

namespace pl::minidfs {

// ============================================================================
// MySQL connection configuration
// ============================================================================

struct MySQLConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database = "minidfs";
    uint32_t pool_size = 16;
};

// ============================================================================
// PooledConnection — RAII handle that returns connection to pool on destruction
// ============================================================================

class MySQLConnectionPool;

class PooledConnection {
public:
    PooledConnection() = default;
    ~PooledConnection();

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    /// Access the underlying connection.
    boost::mysql::any_connection& connection() { return *conn_; }
    boost::mysql::any_connection* operator->() { return conn_.get(); }
    boost::mysql::any_connection& operator*() { return *conn_; }

    /// Execute a SQL statement. Convenience wrapper.
    pl::Result<boost::mysql::results> execute(std::string_view sql);

    explicit operator bool() const { return conn_ != nullptr; }

private:
    friend class MySQLConnectionPool;

    PooledConnection(std::unique_ptr<boost::mysql::any_connection> conn,
                     std::shared_ptr<boost::asio::io_context> io_ctx,
                     MySQLConnectionPool* pool);

    std::unique_ptr<boost::mysql::any_connection> conn_;
    std::shared_ptr<boost::asio::io_context> io_ctx_;
    MySQLConnectionPool* pool_ = nullptr;
};

// ============================================================================
// MySQLConnectionPool — thread-safe connection pool
// ============================================================================

class MySQLConnectionPool : public std::enable_shared_from_this<MySQLConnectionPool> {
public:
    ~MySQLConnectionPool();

    MySQLConnectionPool(const MySQLConnectionPool&) = delete;
    MySQLConnectionPool& operator=(const MySQLConnectionPool&) = delete;

    /// Create a connection pool. Eagerly creates `config.pool_size` connections.
    static pl::Result<std::shared_ptr<MySQLConnectionPool>> create(const MySQLConfig& config);

    /// Acquire a connection from the pool. Blocks if none available.
    pl::Result<PooledConnection> acquire();

    /// Return a connection to the pool (called by PooledConnection destructor).
    void release(std::unique_ptr<boost::mysql::any_connection> conn,
                 std::shared_ptr<boost::asio::io_context> io_ctx);

    /// Get pool size.
    [[nodiscard]] uint32_t pool_size() const { return pool_size_; }

private:
    explicit MySQLConnectionPool(const MySQLConfig& config);

    pl::Result<pl::Void> init();

    /// Create a single connection.
    pl::Result<std::pair<std::unique_ptr<boost::mysql::any_connection>,
                         std::shared_ptr<boost::asio::io_context>>>
    create_connection();

private:
    MySQLConfig config_;
    uint32_t pool_size_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;

    struct PoolEntry {
        std::unique_ptr<boost::mysql::any_connection> conn;
        std::shared_ptr<boost::asio::io_context> io_ctx;
    };
    std::queue<PoolEntry> idle_;
};

} // namespace pl::minidfs
