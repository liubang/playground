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
#include <exception>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

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

struct SplitTotals {
    size_t bytes = 0;
    double wall_time_ms = 0.0;
    double metadata_time_ms = 0.0;
    double split_discovery_time_ms = 0.0;
    double connect_time_ms = 0.0;
    double schema_time_ms = 0.0;
    double sql_build_time_ms = 0.0;
    double execute_time_ms = 0.0;
    double read_time_ms = 0.0;
    double decode_time_ms = 0.0;
    double page_build_time_ms = 0.0;
};

struct PipelineTotals {
    size_t pages = 0;
    bool blocking = false;
};

struct AccumulatorTotals {
    size_t input_rows = 0;
    size_t output_rows = 0;
    size_t groups = 0;
    size_t memory_bytes = 0;
    size_t memory_limit_bytes = 0;
    bool memory_limited = false;
    size_t partial_input_rows = 0;
    size_t final_input_rows = 0;
    double key_time_ms = 0.0;
    double hash_time_ms = 0.0;
    double update_time_ms = 0.0;
    double result_time_ms = 0.0;
    double partial_time_ms = 0.0;
    double final_time_ms = 0.0;
};

SplitTotals split_totals(const pl::flux::execution::ExecutionProfile& profile) {
    SplitTotals totals;
    for (const auto& pipeline : profile.pipelines) {
        for (const auto& split : pipeline.split_stats) {
            totals.bytes += split.bytes_produced;
            totals.wall_time_ms += split.wall_time_ms;
            totals.metadata_time_ms += split.metadata_time_ms;
            totals.split_discovery_time_ms += split.split_discovery_time_ms;
            totals.connect_time_ms += split.connect_time_ms;
            totals.schema_time_ms += split.schema_time_ms;
            totals.sql_build_time_ms += split.sql_build_time_ms;
            totals.execute_time_ms += split.execute_time_ms;
            totals.read_time_ms += split.read_time_ms;
            totals.decode_time_ms += split.decode_time_ms;
            totals.page_build_time_ms += split.page_build_time_ms;
        }
    }
    return totals;
}

AccumulatorTotals accumulator_totals(const pl::flux::execution::ExecutionProfile& profile) {
    AccumulatorTotals totals;
    for (const auto& pipeline : profile.pipelines) {
        for (const auto& accumulator : pipeline.accumulator_stats) {
            totals.input_rows += accumulator.input_rows;
            totals.output_rows += accumulator.output_rows;
            totals.groups += accumulator.groups;
            totals.memory_bytes += accumulator.memory_bytes;
            totals.memory_limit_bytes = std::max(totals.memory_limit_bytes,
                                                 accumulator.memory_limit_bytes);
            totals.memory_limited = totals.memory_limited || accumulator.memory_limited;
            totals.key_time_ms += accumulator.key_time_ms;
            totals.hash_time_ms += accumulator.hash_time_ms;
            totals.update_time_ms += accumulator.update_time_ms;
            totals.result_time_ms += accumulator.result_time_ms;
            const double total_time_ms = accumulator.key_time_ms + accumulator.hash_time_ms +
                                         accumulator.update_time_ms + accumulator.result_time_ms;
            if (accumulator.phase == "partial") {
                totals.partial_input_rows += accumulator.input_rows;
                totals.partial_time_ms += total_time_ms;
            } else if (accumulator.phase == "final") {
                totals.final_input_rows += accumulator.input_rows;
                totals.final_time_ms += total_time_ms;
            }
        }
    }
    return totals;
}

PipelineTotals pipeline_totals(const pl::flux::execution::ExecutionProfile& profile) {
    PipelineTotals totals;
    for (const auto& pipeline : profile.pipelines) {
        totals.pages += pipeline.pages;
        totals.blocking = totals.blocking || pipeline.blocking;
    }
    return totals;
}

int run_benchmark(int argc, char** argv) {
    const int64_t rows = argc > 1 ? std::stoll(argv[1]) : 500000;
    const std::string db_path =
        argc > 2 ? argv[2] : "/tmp/flux_sqlite_scan_" + std::to_string(getpid()) + ".db";
    const std::string scenario = argc > 3 ? argv[3] : "filter_project";
    const double threshold = argc > 4 ? std::stod(argv[4]) : 50.0;
    if (!create_database(db_path, rows)) {
        return 1;
    }

    auto scan = pl::flux::plan::MakeSourceScan("sqlite", "sqlite", db_path, "cpu");
    std::shared_ptr<pl::flux::plan::PlanNode> query = scan;
    if (scenario == "scan") {
        query = scan;
    } else if (scenario == "wide_filter") {
        pl::flux::plan::PredicateSpec predicate;
        predicate.op = pl::flux::plan::PredicateOp::Gte;
        predicate.column = "usage";
        predicate.literal =
            pl::flux::plan::PredicateLiteral{.kind = pl::flux::plan::PredicateLiteralKind::Float,
                                             .float_value = threshold,
                                             .string_value = {}};
        query = pl::flux::plan::MakeProject(pl::flux::plan::MakeFilter(scan, {predicate}),
                                            {"_time", "host", "region", "usage", "seq"});
    } else if (scenario == "topn") {
        pl::flux::plan::SortKey key{.column = "usage", .desc = true};
        query = pl::flux::plan::MakeLimit(pl::flux::plan::MakeSort(scan, {key}), 100, 0);
    } else if (scenario == "group_count") {
        auto materialized =
            pl::flux::plan::MakeMaterializeBarrier(scan, "benchmark memory fallback", "benchmark");
        auto grouped = pl::flux::plan::MakeGroup(materialized, {"host"});
        query = pl::flux::plan::MakeAggregate(grouped, pl::flux::plan::AggregateFunction::Count,
                                              "usage");
    } else if (scenario == "group_sum") {
        auto materialized =
            pl::flux::plan::MakeMaterializeBarrier(scan, "benchmark memory fallback", "benchmark");
        auto grouped = pl::flux::plan::MakeGroup(materialized, {"host"});
        query =
            pl::flux::plan::MakeAggregate(grouped, pl::flux::plan::AggregateFunction::Sum, "usage");
    } else if (scenario == "group_mean") {
        auto materialized =
            pl::flux::plan::MakeMaterializeBarrier(scan, "benchmark memory fallback", "benchmark");
        auto grouped = pl::flux::plan::MakeGroup(materialized, {"host"});
        query = pl::flux::plan::MakeAggregate(grouped, pl::flux::plan::AggregateFunction::Mean,
                                              "usage");
    } else if (scenario == "distinct_host") {
        auto materialized =
            pl::flux::plan::MakeMaterializeBarrier(scan, "benchmark memory fallback", "benchmark");
        query = pl::flux::plan::MakeDistinct(materialized, "host");
    } else {
        pl::flux::plan::PredicateSpec predicate;
        predicate.op = pl::flux::plan::PredicateOp::Gte;
        predicate.column = "usage";
        predicate.literal =
            pl::flux::plan::PredicateLiteral{.kind = pl::flux::plan::PredicateLiteralKind::Float,
                                             .float_value = threshold,
                                             .string_value = {}};
        query = pl::flux::plan::MakeProject(pl::flux::plan::MakeFilter(scan, {predicate}),
                                            {"host", "usage"});
    }

    auto task_or = pl::flux::execution::PhysicalPlanner().Plan(query);
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
    const PipelineTotals pipelines = pipeline_totals(result_or->profile);
    const SplitTotals splits = split_totals(result_or->profile);
    const AccumulatorTotals accumulators = accumulator_totals(result_or->profile);
    std::cout << "{"
              << "\"scenario\":\"" << scenario << "\","
              << "\"rows\":" << rows << ","
              << "\"threshold\":" << threshold << ","
              << "\"drivers\":" << drivers << ","
              << "\"output_rows\":" << output_rows << ","
              << "\"pages\":" << pipelines.pages << ","
              << "\"split_bytes\":" << splits.bytes << ","
              << "\"split_wall_time_ms\":" << splits.wall_time_ms << ","
              << "\"split_metadata_time_ms\":" << splits.metadata_time_ms << ","
              << "\"split_discovery_time_ms\":" << splits.split_discovery_time_ms << ","
              << "\"split_connect_time_ms\":" << splits.connect_time_ms << ","
              << "\"split_schema_time_ms\":" << splits.schema_time_ms << ","
              << "\"split_sql_build_time_ms\":" << splits.sql_build_time_ms << ","
              << "\"split_execute_time_ms\":" << splits.execute_time_ms << ","
              << "\"split_read_time_ms\":" << splits.read_time_ms << ","
              << "\"split_decode_time_ms\":" << splits.decode_time_ms << ","
              << "\"split_page_build_time_ms\":" << splits.page_build_time_ms << ","
              << "\"accumulator_input_rows\":" << accumulators.input_rows << ","
              << "\"accumulator_output_rows\":" << accumulators.output_rows << ","
              << "\"accumulator_groups\":" << accumulators.groups << ","
              << "\"accumulator_memory_bytes\":" << accumulators.memory_bytes << ","
              << "\"accumulator_memory_limit_bytes\":" << accumulators.memory_limit_bytes << ","
              << "\"accumulator_memory_limited\":"
              << (accumulators.memory_limited ? "true" : "false") << ","
              << "\"accumulator_partial_input_rows\":" << accumulators.partial_input_rows << ","
              << "\"accumulator_final_input_rows\":" << accumulators.final_input_rows << ","
              << "\"accumulator_key_time_ms\":" << accumulators.key_time_ms << ","
              << "\"accumulator_hash_time_ms\":" << accumulators.hash_time_ms << ","
              << "\"accumulator_update_time_ms\":" << accumulators.update_time_ms << ","
              << "\"accumulator_result_time_ms\":" << accumulators.result_time_ms << ","
              << "\"accumulator_partial_time_ms\":" << accumulators.partial_time_ms << ","
              << "\"accumulator_final_time_ms\":" << accumulators.final_time_ms << ","
              << "\"blocking\":" << (pipelines.blocking ? "true" : "false") << ","
              << "\"seconds\":" << seconds << ","
              << "\"rows_per_second\":" << static_cast<double>(rows) / std::max(seconds, 0.000001)
              << "}\n";
    std::remove(db_path.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_benchmark(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "sqlite benchmark failed: " << error.what() << "\n";
        return 1;
    }
}
