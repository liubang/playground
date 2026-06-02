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
// Created: 2026/06/02 22:23

#include <sstream>
#include <string>
#include <vector>

#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/execution/physical_executor_internal.h"

namespace pl::flux::execution {

bool IsAccumulatingOperatorName(const std::string& name) {
    return name == "GroupOperator" || name == "DistinctOperator" || name == "AggregateOperator" ||
           name == "PartialAggregateOperator" || name == "FinalAggregateOperator" ||
           name == "PartialGroupOperator" || name == "FinalGroupOperator" ||
           name == "PartialDistinctOperator" || name == "FinalDistinctOperator";
}

std::vector<std::string> AccumulatingOperators(const std::vector<std::string>& operators) {
    std::vector<std::string> result;
    for (const auto& name : operators) {
        if (IsAccumulatingOperatorName(name)) {
            result.push_back(name);
        }
    }
    return result;
}

std::string JsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string JsonStringArray(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << JsonString(values[index]);
    }
    out << "]";
    return out.str();
}

std::string FormatDistribution(const ExchangeDistributionProfile& distribution) {
    std::ostringstream out;
    out << distribution.kind << "(partitions=" << distribution.partitions;
    if (!distribution.partition_keys.empty()) {
        out << ", keys=" << plan::StringList(distribution.partition_keys);
    }
    if (!distribution.heavy_hitters.empty()) {
        out << ", heavy_hitters=" << plan::StringList(distribution.heavy_hitters);
    }
    out << ", include_group_key=" << (distribution.include_group_key ? "true" : "false") << ")";
    return out.str();
}

std::string FormatDistributionJson(const std::optional<ExchangeDistributionProfile>& distribution) {
    if (!distribution.has_value()) {
        return "null";
    }
    std::ostringstream out;
    out << "{";
    out << "\"kind\":" << JsonString(distribution->kind);
    out << ",\"partitionKeys\":" << JsonStringArray(distribution->partition_keys);
    out << ",\"heavyHitters\":" << JsonStringArray(distribution->heavy_hitters);
    out << ",\"includeGroupKey\":" << (distribution->include_group_key ? "true" : "false");
    out << ",\"partitions\":" << distribution->partitions;
    out << "}";
    return out.str();
}

std::string FormatPipelinePlan(const std::shared_ptr<plan::PlanNode>& logical_plan) {
    auto task_or = PhysicalPlanner().Plan(logical_plan);
    if (!task_or.ok()) {
        return task_or.status().ToString() + "\n";
    }
    std::ostringstream out;
    out << "PipelinePlan\n";
    for (const auto& pipeline : task_or->pipelines) {
        const size_t drivers = pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size();
        out << R"(- Pipeline(id=")" << pipeline.id << R"(", name=")" << pipeline.name
            << R"(", role=")" << pipeline.role << "\")\n";
        out << "  drivers: " << drivers << "\n";
        if (!pipeline.dependencies.empty()) {
            out << "  depends_on: " << plan::StringList(pipeline.dependencies) << "\n";
        }
        if (pipeline.distribution.has_value()) {
            out << "  distribution: " << FormatDistribution(*pipeline.distribution) << "\n";
        }
        out << "  blocking: "
            << (internal::HasBlockingOperator(pipeline.operators) ? "true" : "false") << "\n";
        const auto accumulators = AccumulatingOperators(pipeline.operators);
        if (!accumulators.empty()) {
            out << "  accumulators: " << plan::StringList(accumulators) << "\n";
        }
        out << "  operators: " << plan::StringList(pipeline.operators) << "\n";
    }
    return out.str();
}

std::string FormatPipelinePlanJson(const std::shared_ptr<plan::PlanNode>& logical_plan) {
    auto task_or = PhysicalPlanner().Plan(logical_plan);
    if (!task_or.ok()) {
        return absl::StrCat("{\"error\":", JsonString(task_or.status().ToString()), "}");
    }
    std::ostringstream out;
    out << "{\"pipelines\":[";
    for (size_t index = 0; index < task_or->pipelines.size(); ++index) {
        const auto& pipeline = task_or->pipelines[index];
        const size_t drivers = pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size();
        if (index != 0) {
            out << ",";
        }
        out << "{";
        out << "\"id\":" << JsonString(pipeline.id);
        out << ",\"name\":" << JsonString(pipeline.name);
        out << ",\"role\":" << JsonString(pipeline.role);
        out << ",\"drivers\":" << drivers;
        out << ",\"dependencies\":" << JsonStringArray(pipeline.dependencies);
        out << ",\"distribution\":" << FormatDistributionJson(pipeline.distribution);
        out << ",\"blocking\":"
            << (internal::HasBlockingOperator(pipeline.operators) ? "true" : "false");
        out << ",\"accumulators\":" << JsonStringArray(AccumulatingOperators(pipeline.operators));
        out << ",\"operators\":" << JsonStringArray(pipeline.operators);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string EscapeMermaidLabel(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "<br/>";
                break;
            case '|':
                out << "&#124;";
                break;
            default:
                out << c;
                break;
        }
    }
    return out.str();
}

std::string PipelineMermaidLabel(const Pipeline& pipeline) {
    std::ostringstream label;
    const size_t drivers = pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size();
    label << pipeline.id << "\nrole: " << pipeline.role << "\ndrivers: " << drivers;
    label << "\nblocking: "
          << (internal::HasBlockingOperator(pipeline.operators) ? "true" : "false");
    if (pipeline.distribution.has_value()) {
        label << "\ndistribution: " << FormatDistribution(*pipeline.distribution);
    }
    if (!pipeline.operators.empty()) {
        label << "\noperators: " << plan::StringList(pipeline.operators);
    }
    return label.str();
}

std::string FormatPipelinePlanMermaid(const std::shared_ptr<plan::PlanNode>& logical_plan) {
    auto task_or = PhysicalPlanner().Plan(logical_plan);
    if (!task_or.ok()) {
        return absl::StrCat(
            "flowchart TD\n  p0[\"", EscapeMermaidLabel(task_or.status().ToString()), "\"]\n");
    }

    std::ostringstream out;
    out << "flowchart TD\n";
    std::unordered_map<std::string, std::string> ids;
    ids.reserve(task_or->pipelines.size());
    for (size_t index = 0; index < task_or->pipelines.size(); ++index) {
        const auto& pipeline = task_or->pipelines[index];
        const std::string id = "p" + std::to_string(index);
        ids.emplace(pipeline.id, id);
        out << "  " << id << "[\"" << EscapeMermaidLabel(PipelineMermaidLabel(pipeline)) << "\"]\n";
    }
    for (const auto& pipeline : task_or->pipelines) {
        auto target = ids.find(pipeline.id);
        if (target == ids.end()) {
            continue;
        }
        for (const auto& dependency : pipeline.dependencies) {
            auto source = ids.find(dependency);
            if (source != ids.end()) {
                out << "  " << source->second << " --> " << target->second << "\n";
            }
        }
    }
    return out.str();
}

std::string FormatSplitStats(const std::vector<connector::ConnectorSplitStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "{id=" << stats[i].split_id << ", pages=" << stats[i].pages_produced
            << ", rows=" << stats[i].rows_produced << ", bytes=" << stats[i].bytes_produced
            << ", wall_ms=" << stats[i].wall_time_ms
            << ", metadata_ms=" << stats[i].metadata_time_ms
            << ", split_ms=" << stats[i].split_discovery_time_ms
            << ", connect_ms=" << stats[i].connect_time_ms
            << ", schema_ms=" << stats[i].schema_time_ms
            << ", sql_ms=" << stats[i].sql_build_time_ms
            << ", execute_ms=" << stats[i].execute_time_ms << ", read_ms=" << stats[i].read_time_ms
            << ", decode_ms=" << stats[i].decode_time_ms
            << ", page_ms=" << stats[i].page_build_time_ms
            << ", finished=" << (stats[i].finished ? "true" : "false") << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatAccumulatorStats(const std::vector<AccumulatorStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "{operator=" << stats[i].operator_name << ", mode=" << stats[i].mode
            << ", phase=" << stats[i].phase << ", key=" << stats[i].key_strategy
            << ", input_rows=" << stats[i].input_rows << ", output_rows=" << stats[i].output_rows
            << ", groups=" << stats[i].groups << ", memory_bytes=" << stats[i].memory_bytes
            << ", memory_limit_bytes=" << stats[i].memory_limit_bytes
            << ", memory_limited=" << (stats[i].memory_limited ? "true" : "false")
            << ", key_ms=" << stats[i].key_time_ms << ", hash_ms=" << stats[i].hash_time_ms
            << ", update_ms=" << stats[i].update_time_ms
            << ", result_ms=" << stats[i].result_time_ms << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatExchangePartitionStats(const std::vector<ExchangePartitionStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t index = 0; index < stats.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << "{partition=" << stats[index].partition << ", rows=" << stats[index].rows
            << ", bytes=" << stats[index].bytes << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatExecutionProfile(const ExecutionProfile& profile) {
    std::ostringstream out;
    out << "ExecutionProfile\n";
    out << "memory: used_bytes=" << profile.memory.used_bytes
        << ", peak_bytes=" << profile.memory.peak_bytes
        << ", limit_bytes=" << profile.memory.limit_bytes
        << ", limited=" << (profile.memory.limited ? "true" : "false") << "\n";
    for (const auto& pipeline : profile.pipelines) {
        out << R"(- Pipeline(id=")" << pipeline.id << R"(", name=")" << pipeline.name
            << R"(", role=")" << pipeline.role << "\")\n";
        if (!pipeline.dependencies.empty()) {
            out << "  depends_on: " << plan::StringList(pipeline.dependencies) << "\n";
        }
        if (pipeline.distribution.has_value()) {
            out << "  distribution: " << FormatDistribution(*pipeline.distribution) << "\n";
        }
        out << "  drivers: " << pipeline.drivers << "\n";
        out << "  blocking: " << (pipeline.blocking ? "true" : "false") << "\n";
        const auto accumulators = AccumulatingOperators(pipeline.operators);
        if (!accumulators.empty()) {
            out << "  accumulators: " << plan::StringList(accumulators) << "\n";
        }
        out << "  stats: pages=" << pipeline.pages << ", rows=" << pipeline.rows
            << ", blocked=" << (pipeline.blocked ? "true" : "false")
            << ", finished=" << (pipeline.finished ? "true" : "false") << "\n";
        if (!pipeline.split_stats.empty()) {
            out << "  splits: " << FormatSplitStats(pipeline.split_stats) << "\n";
        }
        if (!pipeline.accumulator_stats.empty()) {
            out << "  accumulator_stats: " << FormatAccumulatorStats(pipeline.accumulator_stats)
                << "\n";
        }
        if (!pipeline.exchange_partition_stats.empty()) {
            out << "  exchange_partition_stats: "
                << FormatExchangePartitionStats(pipeline.exchange_partition_stats) << "\n";
        }
        if (!pipeline.error.empty()) {
            out << "  error: " << pipeline.error << "\n";
        }
    }
    return out.str();
}

std::string FormatSplitStatsJson(const std::vector<connector::ConnectorSplitStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "{";
        out << "\"id\":" << stats[i].split_id;
        out << ",\"pages\":" << stats[i].pages_produced;
        out << ",\"rows\":" << stats[i].rows_produced;
        out << ",\"bytes\":" << stats[i].bytes_produced;
        out << ",\"wallMs\":" << stats[i].wall_time_ms;
        out << ",\"metadataMs\":" << stats[i].metadata_time_ms;
        out << ",\"splitMs\":" << stats[i].split_discovery_time_ms;
        out << ",\"connectMs\":" << stats[i].connect_time_ms;
        out << ",\"schemaMs\":" << stats[i].schema_time_ms;
        out << ",\"sqlMs\":" << stats[i].sql_build_time_ms;
        out << ",\"executeMs\":" << stats[i].execute_time_ms;
        out << ",\"readMs\":" << stats[i].read_time_ms;
        out << ",\"decodeMs\":" << stats[i].decode_time_ms;
        out << ",\"pageMs\":" << stats[i].page_build_time_ms;
        out << ",\"finished\":" << (stats[i].finished ? "true" : "false");
        out << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatAccumulatorStatsJson(const std::vector<AccumulatorStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "{";
        out << "\"operator\":" << JsonString(stats[i].operator_name);
        out << ",\"mode\":" << JsonString(stats[i].mode);
        out << ",\"phase\":" << JsonString(stats[i].phase);
        out << ",\"key\":" << JsonString(stats[i].key_strategy);
        out << ",\"inputRows\":" << stats[i].input_rows;
        out << ",\"outputRows\":" << stats[i].output_rows;
        out << ",\"groups\":" << stats[i].groups;
        out << ",\"memoryBytes\":" << stats[i].memory_bytes;
        out << ",\"memoryLimitBytes\":" << stats[i].memory_limit_bytes;
        out << ",\"memoryLimited\":" << (stats[i].memory_limited ? "true" : "false");
        out << ",\"keyMs\":" << stats[i].key_time_ms;
        out << ",\"hashMs\":" << stats[i].hash_time_ms;
        out << ",\"updateMs\":" << stats[i].update_time_ms;
        out << ",\"resultMs\":" << stats[i].result_time_ms;
        out << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatExchangePartitionStatsJson(const std::vector<ExchangePartitionStats>& stats) {
    std::ostringstream out;
    out << "[";
    for (size_t index = 0; index < stats.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << "{";
        out << "\"partition\":" << stats[index].partition;
        out << ",\"rows\":" << stats[index].rows;
        out << ",\"bytes\":" << stats[index].bytes;
        out << "}";
    }
    out << "]";
    return out.str();
}

std::string FormatExecutionProfileJson(const ExecutionProfile& profile) {
    std::ostringstream out;
    out << "{\"memory\":{";
    out << "\"usedBytes\":" << profile.memory.used_bytes;
    out << ",\"peakBytes\":" << profile.memory.peak_bytes;
    out << ",\"limitBytes\":" << profile.memory.limit_bytes;
    out << ",\"limited\":" << (profile.memory.limited ? "true" : "false");
    out << "},\"pipelines\":[";
    for (size_t index = 0; index < profile.pipelines.size(); ++index) {
        const auto& pipeline = profile.pipelines[index];
        if (index != 0) {
            out << ",";
        }
        out << "{";
        out << "\"id\":" << JsonString(pipeline.id);
        out << ",\"name\":" << JsonString(pipeline.name);
        out << ",\"role\":" << JsonString(pipeline.role);
        out << ",\"dependencies\":" << JsonStringArray(pipeline.dependencies);
        out << ",\"distribution\":" << FormatDistributionJson(pipeline.distribution);
        out << ",\"drivers\":" << pipeline.drivers;
        out << ",\"blocking\":" << (pipeline.blocking ? "true" : "false");
        out << ",\"operators\":" << JsonStringArray(pipeline.operators);
        out << ",\"accumulators\":" << JsonStringArray(AccumulatingOperators(pipeline.operators));
        out << ",\"pages\":" << pipeline.pages;
        out << ",\"rows\":" << pipeline.rows;
        out << ",\"blocked\":" << (pipeline.blocked ? "true" : "false");
        out << ",\"finished\":" << (pipeline.finished ? "true" : "false");
        out << ",\"error\":" << JsonString(pipeline.error);
        out << ",\"splits\":" << FormatSplitStatsJson(pipeline.split_stats);
        out << ",\"accumulatorStats\":" << FormatAccumulatorStatsJson(pipeline.accumulator_stats);
        out << ",\"exchangePartitionStats\":"
            << FormatExchangePartitionStatsJson(pipeline.exchange_partition_stats);
        out << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace pl::flux::execution
