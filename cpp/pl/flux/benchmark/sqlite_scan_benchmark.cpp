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
// Created: 2026/05/16 09:37

#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

namespace {

int exec_sql(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite exec failed: " << (error == nullptr ? "unknown" : error) << "\n";
        sqlite3_free(error);
    }
    return rc;
}

bool create_database(const std::string& path, int64_t rows) {
    std::remove(path.c_str());
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "sqlite open failed\n";
        return false;
    }
    if (exec_sql(db, "PRAGMA journal_mode=OFF;"
                     "PRAGMA synchronous=OFF;"
                     "CREATE TABLE cpu("
                     "_time TEXT NOT NULL,"
                     "host TEXT NOT NULL,"
                     "region TEXT NOT NULL,"
                     "usage REAL NOT NULL,"
                     "seq INTEGER NOT NULL);"
                     "BEGIN;") != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* insert = "INSERT INTO cpu(_time, host, region, usage, seq) VALUES (?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, insert, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "sqlite prepare insert failed\n";
        sqlite3_close(db);
        return false;
    }
    for (int64_t index = 0; index < rows; ++index) {
        const std::string time = "2024-07-01T10:" + std::to_string(index % 60) + ":00Z";
        const std::string host = "edge-" + std::to_string(index % 64);
        const std::string region = index % 2 == 0 ? "west" : "east";
        sqlite3_bind_text(stmt, 1, time.c_str(), static_cast<int>(time.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, host.c_str(), static_cast<int>(host.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, region.c_str(), static_cast<int>(region.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, static_cast<double>(index % 100));
        sqlite3_bind_int64(stmt, 5, index);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "sqlite insert failed\n";
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    const bool ok = exec_sql(db, "COMMIT; CREATE INDEX cpu_usage_idx ON cpu(usage);") == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const int64_t rows = argc > 1 ? std::stoll(argv[1]) : 500000;
    const std::string db_path =
        argc > 2 ? argv[2] : "/tmp/flux_sqlite_scan_" + std::to_string(getpid()) + ".db";
    if (!create_database(db_path, rows)) {
        return 1;
    }

    auto scan = pl::flux::plan::MakeSourceScan("sqlite", "sqlite", db_path, "cpu");
    pl::flux::plan::PredicateSpec predicate;
    predicate.op = pl::flux::plan::PredicateOp::Gte;
    predicate.column = "usage";
    predicate.literal =
        pl::flux::plan::PredicateLiteral{.kind = pl::flux::plan::PredicateLiteralKind::Float,
                                         .float_value = 50.0,
                                         .string_value = {}};
    auto filtered = pl::flux::plan::MakeFilter(scan, {predicate});
    auto projected = pl::flux::plan::MakeProject(filtered, {"host", "usage"});

    auto task_or = pl::flux::execution::PhysicalPlanner().Plan(projected);
    if (!task_or.ok()) {
        std::cerr << task_or.status() << "\n";
        return 1;
    }
    const size_t drivers =
        task_or->pipelines.empty() ? 0 : task_or->pipelines[0].driver_roots.size();

    const auto started = std::chrono::steady_clock::now();
    auto result_or = pl::flux::execution::Scheduler().RunWithProfile(std::move(*task_or));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!result_or.ok()) {
        std::cerr << result_or.status() << "\n";
        return 1;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const size_t output_rows = result_or->value.as_table().rows.size();
    const auto& profile = result_or->profile.pipelines.front();
    std::cout << "{"
              << "\"rows\":" << rows << ","
              << "\"drivers\":" << drivers << ","
              << "\"output_rows\":" << output_rows << ","
              << "\"pages\":" << profile.pages << ","
              << "\"blocking\":" << (profile.blocking ? "true" : "false") << ","
              << "\"seconds\":" << seconds << ","
              << "\"rows_per_second\":" << static_cast<double>(rows) / std::max(seconds, 0.000001)
              << "}\n";
    std::remove(db_path.c_str());
    return 0;
}
