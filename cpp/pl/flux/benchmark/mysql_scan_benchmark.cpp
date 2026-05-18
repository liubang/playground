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
// Created: 2026/05/16 10:33

#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string arg_or_env(int argc, char** argv, int index, const char* env_name) {
    if (argc > index && argv[index] != nullptr && !std::string(argv[index]).empty()) {
        return argv[index];
    }
    const char* value = std::getenv(env_name);
    return value == nullptr ? std::string() : std::string(value);
}

pl::flux::plan::PredicateSpec usage_predicate(double threshold) {
    pl::flux::plan::PredicateSpec predicate;
    predicate.op = pl::flux::plan::PredicateOp::Gte;
    predicate.column = "usage";
    predicate.literal =
        pl::flux::plan::PredicateLiteral{.kind = pl::flux::plan::PredicateLiteralKind::Float,
                                         .float_value = threshold,
                                         .string_value = {}};
    return predicate;
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

} // namespace

int main(int argc, char** argv) {
    const std::string dsn = arg_or_env(argc, argv, 1, "FLUX_MYSQL_TEST_DSN");
    if (dsn.empty()) {
        std::cerr << "usage: mysql_scan_benchmark <dsn|$FLUX_MYSQL_TEST_DSN> [table] [scenario] "
                     "[threshold]\n";
        return 2;
    }
    const std::string table = argc > 2 ? argv[2] : "cpu";
    const std::string scenario = argc > 3 ? argv[3] : "filter_project";
    const double threshold = argc > 4 ? std::stod(argv[4]) : 50.0;

    auto scan = pl::flux::plan::MakeSourceScan("mysql", "mysql", dsn, table);
    std::shared_ptr<pl::flux::plan::PlanNode> query = scan;
    if (scenario == "scan") {
        query = scan;
    } else if (scenario == "topn") {
        pl::flux::plan::SortKey key{.column = "usage", .desc = true};
        query = pl::flux::plan::MakeLimit(pl::flux::plan::MakeSort(scan, {key}), 100, 0);
    } else if (scenario == "wide_filter") {
        query = pl::flux::plan::MakeProject(
            pl::flux::plan::MakeFilter(scan, {usage_predicate(threshold)}),
            {"_time", "host", "region", "usage"});
    } else {
        query = pl::flux::plan::MakeProject(
            pl::flux::plan::MakeFilter(scan, {usage_predicate(threshold)}), {"host", "usage"});
    }

    auto task_or = pl::flux::execution::PhysicalPlanner().Plan(query);
    if (!task_or.ok()) {
        std::cerr << task_or.status() << "\n";
        return 1;
    }
    size_t drivers = 0;
    for (const auto& pipeline : task_or->pipelines) {
        drivers += pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size();
    }

    size_t output_rows = 0;
    size_t output_pages = 0;
    const auto started = std::chrono::steady_clock::now();
    auto result_or = pl::flux::execution::Scheduler().RunToSink(std::move(*task_or),
                                                                [&](const pl::flux::Page& page) {
                                                                    ++output_pages;
                                                                    output_rows += page.row_count();
                                                                    return absl::OkStatus();
                                                                });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!result_or.ok()) {
        std::cerr << result_or.status() << "\n";
        return 1;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const SplitTotals splits = split_totals(result_or->profile);
    std::cout << "{"
              << R"("scenario":")" << scenario << R"(",)"
              << R"("table":")" << table << R"(",)"
              << R"("threshold":)" << threshold << ","
              << R"("drivers":)" << drivers << ","
              << R"("output_rows":)" << output_rows << ","
              << R"("output_pages":)" << output_pages << ","
              << R"("split_bytes":)" << splits.bytes << ","
              << R"("split_wall_time_ms":)" << splits.wall_time_ms << ","
              << R"("split_metadata_time_ms":)" << splits.metadata_time_ms << ","
              << R"("split_discovery_time_ms":)" << splits.split_discovery_time_ms << ","
              << R"("split_connect_time_ms":)" << splits.connect_time_ms << ","
              << R"("split_schema_time_ms":)" << splits.schema_time_ms << ","
              << R"("split_sql_build_time_ms":)" << splits.sql_build_time_ms << ","
              << R"("split_execute_time_ms":)" << splits.execute_time_ms << ","
              << R"("split_read_time_ms":)" << splits.read_time_ms << ","
              << R"("split_decode_time_ms":)" << splits.decode_time_ms << ","
              << R"("split_page_build_time_ms":)" << splits.page_build_time_ms << ","
              << R"("query_memory_used_bytes":)" << result_or->profile.memory.used_bytes << ","
              << R"("query_memory_peak_bytes":)" << result_or->profile.memory.peak_bytes << ","
              << R"("query_memory_limit_bytes":)" << result_or->profile.memory.limit_bytes << ","
              << R"("query_memory_limited":)"
              << (result_or->profile.memory.limited ? "true" : "false") << ","
              << R"("seconds":)" << seconds << ","
              << R"("output_rows_per_second":)"
              << static_cast<double>(output_rows) / std::max(seconds, 0.000001) << "}\n";
    return 0;
}
