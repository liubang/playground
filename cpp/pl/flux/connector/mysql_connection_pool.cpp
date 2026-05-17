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

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/mysql_source.h"
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/metadata_mode.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <exception>
#include <utility>

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

absl::Status connect_entry(MySQLConnectionPool::Entry* entry, const std::string& dsn) {
    auto config_or = ParseMySQLDsn(dsn);
    if (!config_or.ok()) {
        return config_or.status();
    }

    mysql::connect_params params;
    params.server_address.emplace_host_and_port(config_or->host, config_or->port);
    params.username = config_or->user;
    params.password = config_or->password;
    params.database = config_or->database;
    params.ssl = config_or->ssl ? mysql::ssl_mode::enable : mysql::ssl_mode::disable;

    try {
        entry->conn.emplace(entry->ctx);
        entry->conn->connect(params);
        entry->conn->set_meta_mode(mysql::metadata_mode::full);
        mysql::results result;
        entry->conn->execute("SET time_zone = '+00:00'", result);
        entry->ctx.restart();
        entry->healthy = true;
        return absl::OkStatus();
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql connect failed: ", mysql_error_message(err)));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(absl::StrCat("mysql connect failed: ", err.what()));
    }
}

} // namespace

MySQLConnectionPool::Lease::Lease(MySQLConnectionPool* pool,
                                  std::shared_ptr<Entry> entry,
                                  bool pooled)
    : pool_(pool), entry_(std::move(entry)), pooled_(pooled), released_(false) {}

MySQLConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_),
      entry_(std::move(other.entry_)),
      pooled_(other.pooled_),
      released_(other.released_) {
    other.pool_ = nullptr;
    other.pooled_ = false;
    other.released_ = true;
}

MySQLConnectionPool::Lease& MySQLConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    pool_ = other.pool_;
    entry_ = std::move(other.entry_);
    pooled_ = other.pooled_;
    released_ = other.released_;
    other.pool_ = nullptr;
    other.pooled_ = false;
    other.released_ = true;
    return *this;
}

MySQLConnectionPool::Lease::~Lease() { Release(); }

mysql::any_connection* MySQLConnectionPool::Lease::connection() const {
    if (entry_ == nullptr || !entry_->conn.has_value()) {
        return nullptr;
    }
    return &*entry_->conn;
}

void MySQLConnectionPool::Lease::MarkBroken() {
    if (entry_ != nullptr) {
        entry_->healthy = false;
    }
}

void MySQLConnectionPool::Lease::Release() {
    if (released_) {
        return;
    }
    released_ = true;
    if (pool_ != nullptr && entry_ != nullptr) {
        pool_->Return(std::move(entry_), pooled_);
    }
    pool_ = nullptr;
    pooled_ = false;
}

MySQLConnectionPool::MySQLConnectionPool(std::string dsn, size_t max_idle_connections)
    : dsn_(std::move(dsn)), max_idle_connections_(max_idle_connections) {}

MySQLConnectionPool::~MySQLConnectionPool() = default;

absl::StatusOr<MySQLConnectionPool::Lease> MySQLConnectionPool::Acquire() {
    if (max_idle_connections_ > 0) {
        std::lock_guard<std::mutex> lock(mu_);
        while (!idle_.empty()) {
            auto entry = std::move(idle_.back());
            idle_.pop_back();
            if (entry != nullptr && entry->healthy) {
                entry->ctx.restart();
                return Lease(this, std::move(entry), true);
            }
        }
    }

    auto entry = std::make_shared<Entry>();
    auto status = connect_entry(entry.get(), dsn_);
    if (!status.ok()) {
        return status;
    }
    return Lease(this, std::move(entry), max_idle_connections_ > 0);
}

void MySQLConnectionPool::Return(std::shared_ptr<Entry> entry, bool pooled) {
    if (!pooled || entry == nullptr || !entry->healthy || max_idle_connections_ == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_.size() < max_idle_connections_) {
        idle_.push_back(std::move(entry));
    }
}

absl::StatusOr<std::shared_ptr<MySQLConnectionPool>> MakeMySQLConnectionPool(
    std::string dsn, size_t max_idle_connections) {
    auto config_or = ParseMySQLDsn(dsn);
    if (!config_or.ok()) {
        return config_or.status();
    }
    return std::make_shared<MySQLConnectionPool>(std::move(dsn), max_idle_connections);
}

} // namespace pl::flux::connector
