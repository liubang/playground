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
// Created: 2026/05/17 00:18

#include "cpp/pl/flux/connector/mysql_connection_pool.h"

#include <algorithm>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/error_code.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/metadata_mode.hpp>
#include <boost/mysql/pool_params.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <future>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/mysql_source.h"

namespace pl::flux::connector {
namespace {

namespace mysql = boost::mysql;

std::string mysql_error_message(const mysql::error_with_diagnostics& err) {
    const auto server_message = err.get_diagnostics().server_message();
    if (server_message.empty()) {
        return err.what();
    }
    return absl::StrCat(err.what(), ": ", std::string(server_message));
}

absl::StatusOr<mysql::pool_params> pool_params_from_dsn(const std::string& dsn,
                                                        size_t max_pool_size) {
    auto config_or = ParseMySQLDsn(dsn);
    if (!config_or.ok()) {
        return config_or.status();
    }

    mysql::pool_params params;
    params.server_address.emplace_host_and_port(config_or->host, config_or->port);
    params.username = config_or->user;
    params.password = config_or->password;
    params.database = config_or->database;
    params.ssl = config_or->ssl ? mysql::ssl_mode::enable : mysql::ssl_mode::disable;
    params.initial_size = 0;
    params.max_size = std::max<size_t>(1, max_pool_size);
    params.thread_safe = true;
    return params;
}

} // namespace

struct MySQLBoostConnectionPool::Impl {
    explicit Impl(mysql::pool_params params) : ctx(1), pool(ctx, std::move(params)) {
        pool.async_run(boost::asio::detached);
    }

    ~Impl() {
        pool.cancel();
        ctx.stop();
        ctx.join();
    }

    boost::asio::thread_pool ctx;
    mysql::connection_pool pool;
};

MySQLBoostConnectionPool::Lease::Lease(mysql::pooled_connection conn) : conn_(std::move(conn)) {}

MySQLBoostConnectionPool::Lease::Lease(Lease&& other) noexcept : conn_(std::move(other.conn_)) {}

MySQLBoostConnectionPool::Lease& MySQLBoostConnectionPool::Lease::operator=(
    Lease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    conn_ = std::move(other.conn_);
    return *this;
}

MySQLBoostConnectionPool::Lease::~Lease() {
    Release();
}

mysql::any_connection* MySQLBoostConnectionPool::Lease::connection() {
    if (!conn_.has_value() || !conn_->valid()) {
        return nullptr;
    }
    return &conn_->get();
}

void MySQLBoostConnectionPool::Lease::MarkBroken() {
    if (auto* conn = connection(); conn != nullptr) {
        mysql::error_code err;
        mysql::diagnostics diag;
        conn->close(err, diag);
    }
}

void MySQLBoostConnectionPool::Lease::Release() {
    conn_.reset();
}

MySQLBoostConnectionPool::MySQLBoostConnectionPool(std::string dsn, size_t max_pool_size)
    : dsn_(std::move(dsn)), max_pool_size_(std::max<size_t>(1, max_pool_size)) {
    auto params_or = pool_params_from_dsn(dsn_, max_pool_size_);
    if (!params_or.ok()) {
        return;
    }
    impl_ = std::make_unique<Impl>(std::move(*params_or));
}

MySQLBoostConnectionPool::~MySQLBoostConnectionPool() = default;

absl::StatusOr<MySQLBoostConnectionPool::Lease> MySQLBoostConnectionPool::Acquire() {
    if (impl_ == nullptr) {
        return absl::InvalidArgumentError("mysql boost connection pool is not initialized");
    }
    try {
        std::future<mysql::pooled_connection> future =
            impl_->pool.async_get_connection(boost::asio::use_future);
        mysql::pooled_connection conn = future.get();
        conn->set_meta_mode(mysql::metadata_mode::full);
        mysql::results result;
        conn->execute("SET time_zone = '+00:00'", result);
        return Lease(std::move(conn));
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql pooled connection failed: ", mysql_error_message(err)));
    } catch (const boost::system::system_error& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql pooled connection failed: ", err.what()));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql pooled connection failed: ", err.what()));
    }
}

absl::StatusOr<std::shared_ptr<MySQLBoostConnectionPool>> MakeMySQLBoostConnectionPool(
    std::string dsn, size_t max_pool_size) {
    auto params_or = pool_params_from_dsn(dsn, max_pool_size);
    if (!params_or.ok()) {
        return params_or.status();
    }
    return std::make_shared<MySQLBoostConnectionPool>(std::move(dsn), max_pool_size);
}

} // namespace pl::flux::connector
