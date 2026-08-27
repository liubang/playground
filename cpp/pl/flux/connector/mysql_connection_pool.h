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

#include <boost/mysql/connection_pool.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace pl::flux::connector {

// Formats a boost::mysql error, appending the server diagnostics message when
// available. Shared by the connection pool and the MySQL source.
inline std::string MySQLErrorMessage(const boost::mysql::error_with_diagnostics& err) {
    const auto server_message = err.get_diagnostics().server_message();
    if (server_message.empty()) {
        return err.what();
    }
    return absl::StrCat(err.what(), ": ", std::string(server_message));
}

class MySQLBoostConnectionPool final {
public:
    class Lease final {
    public:
        Lease() = default;
        explicit Lease(boost::mysql::pooled_connection conn);
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] boost::mysql::any_connection* connection();
        void MarkBroken();
        void Release();

    private:
        std::optional<boost::mysql::pooled_connection> conn_;
    };

    // Bounded wait for a pooled connection: boost::mysql's pool keeps
    // retrying forever when the server rejects credentials, so acquiring
    // without a timeout would hang the query indefinitely.
    static constexpr std::chrono::milliseconds kDefaultAcquireTimeout{30000};

    MySQLBoostConnectionPool(std::string dsn,
                             size_t max_pool_size,
                             std::chrono::milliseconds acquire_timeout = kDefaultAcquireTimeout);
    ~MySQLBoostConnectionPool();

    [[nodiscard]] absl::StatusOr<Lease> Acquire();
    [[nodiscard]] size_t max_pool_size() const { return max_pool_size_; }

private:
    struct Impl;
    std::string dsn_;
    size_t max_pool_size_ = 0;
    std::chrono::milliseconds acquire_timeout_ = kDefaultAcquireTimeout;
    std::string init_error_;
    std::unique_ptr<Impl> impl_;
};

absl::StatusOr<std::shared_ptr<MySQLBoostConnectionPool>> MakeMySQLBoostConnectionPool(
    std::string dsn,
    size_t max_pool_size,
    std::chrono::milliseconds acquire_timeout = MySQLBoostConnectionPool::kDefaultAcquireTimeout);

} // namespace pl::flux::connector
