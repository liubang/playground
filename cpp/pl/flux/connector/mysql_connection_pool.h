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
// Created: 2026/05/17 00:17

#pragma once

#include "absl/status/statusor.h"
#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pl::flux::connector {

class MySQLConnectionPool final {
public:
    struct Entry {
        boost::asio::io_context ctx;
        boost::mysql::any_connection conn;
        bool healthy = true;

        Entry() : conn(ctx) {}
    };

    class Lease final {
    public:
        Lease() = default;
        Lease(MySQLConnectionPool* pool, std::shared_ptr<Entry> entry, bool pooled);
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] boost::mysql::any_connection* connection() const;
        void MarkBroken();
        void Release();

    private:
        MySQLConnectionPool* pool_ = nullptr;
        std::shared_ptr<Entry> entry_;
        bool pooled_ = false;
        bool released_ = true;
    };

    MySQLConnectionPool(std::string dsn, size_t max_idle_connections);
    ~MySQLConnectionPool();

    [[nodiscard]] absl::StatusOr<Lease> Acquire();
    [[nodiscard]] size_t max_idle_connections() const { return max_idle_connections_; }

private:
    friend class Lease;

    void Return(std::shared_ptr<Entry> entry, bool pooled);

    std::string dsn_;
    size_t max_idle_connections_ = 0;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Entry>> idle_;
};

absl::StatusOr<std::shared_ptr<MySQLConnectionPool>> MakeMySQLConnectionPool(
    std::string dsn, size_t max_idle_connections);

} // namespace pl::flux::connector
