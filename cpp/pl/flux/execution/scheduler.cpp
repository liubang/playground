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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/execution/accumulator.h"
#include "cpp/pl/flux/execution/page_budget.h"
#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/execution/physical_executor_internal.h"
#include "cpp/pl/flux/execution/task_executor.h"
#include "cpp/pl/flux/optimizer/cbo.h"
#include "cpp/pl/flux/runtime/runtime_builtin_aggregate_helpers.h"
#include "cpp/pl/flux/runtime/runtime_builtin_table_helpers.h"

namespace pl::flux::execution {

void ResetPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats) {
    if (stats == nullptr) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    stats->pages = 0;
    stats->rows = 0;
    stats->split_stats.clear();
    stats->accumulator_stats.clear();
    stats->exchange_partition_stats.clear();
    stats->blocked = false;
    stats->finished = false;
    stats->error.clear();
}

void AddPipelineStatsPage(const std::shared_ptr<Pipeline::Stats>& stats,
                          const runtime::Page& page) {
    if (stats == nullptr) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    ++stats->pages;
    stats->rows += page.row_count();
}

void FinishPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats) {
    if (stats == nullptr) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    stats->finished = true;
}

void AddPipelineSplitStats(const std::shared_ptr<Pipeline::Stats>& stats,
                           const std::vector<connector::ConnectorSplitStats>& split_stats) {
    if (stats == nullptr || split_stats.empty()) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    stats->split_stats.insert(stats->split_stats.end(), split_stats.begin(), split_stats.end());
}

void AddPipelineAccumulatorStats(const std::shared_ptr<Pipeline::Stats>& stats,
                                 const std::vector<AccumulatorStats>& accumulator_stats) {
    if (stats == nullptr || accumulator_stats.empty()) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    stats->accumulator_stats.insert(
        stats->accumulator_stats.end(), accumulator_stats.begin(), accumulator_stats.end());
}

void AddPipelineExchangePartitionStats(
    const std::shared_ptr<Pipeline::Stats>& stats,
    const std::vector<ExchangePartitionStats>& exchange_partition_stats) {
    if (stats == nullptr || exchange_partition_stats.empty()) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    for (const auto& partition : exchange_partition_stats) {
        if (stats->exchange_partition_stats.size() <= partition.partition) {
            stats->exchange_partition_stats.resize(partition.partition + 1);
        }
        auto& target = stats->exchange_partition_stats[partition.partition];
        target.partition = partition.partition;
        target.rows += partition.rows;
        target.bytes += partition.bytes;
    }
}

void FailPipelineStats(const std::shared_ptr<Pipeline::Stats>& stats, const absl::Status& status) {
    if (stats == nullptr) {
        return;
    }
    std::scoped_lock lock(stats->mu);
    stats->blocked = status.code() == absl::StatusCode::kUnavailable;
    stats->error = status.ToString();
    stats->finished = true;
}

Driver::Driver(Pipeline pipeline) : pipeline_(std::move(pipeline)) {}

absl::Status Driver::RunToSink(const PageSink& sink) const {
    if (pipeline_.root == nullptr) {
        return absl::InvalidArgumentError("pipeline has no root operator");
    }
    while (true) {
        auto page_or = pipeline_.root->NextPage();
        if (!page_or.ok()) {
            pipeline_.root->Cancel();
            FailPipelineStats(pipeline_.stats, page_or.status());
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        AddPipelineStatsPage(pipeline_.stats, page_or->value());
        auto status = sink(std::move(page_or->value()));
        if (!status.ok()) {
            pipeline_.root->Cancel();
            FailPipelineStats(pipeline_.stats, status);
            return status;
        }
    }
    std::vector<connector::ConnectorSplitStats> split_stats;
    pipeline_.root->CollectSplitStats(&split_stats);
    AddPipelineSplitStats(pipeline_.stats, split_stats);
    std::vector<AccumulatorStats> accumulator_stats;
    pipeline_.root->CollectAccumulatorStats(&accumulator_stats);
    AddPipelineAccumulatorStats(pipeline_.stats, accumulator_stats);
    std::vector<ExchangePartitionStats> exchange_partition_stats;
    pipeline_.root->CollectExchangePartitionStats(&exchange_partition_stats);
    AddPipelineExchangePartitionStats(pipeline_.stats, exchange_partition_stats);
    FinishPipelineStats(pipeline_.stats);
    return absl::OkStatus();
}

absl::StatusOr<runtime::Value> Driver::Run() const {
    std::optional<runtime::Page> output;
    auto status = RunToSink([&](runtime::Page page) {
        if (!output.has_value()) {
            output = std::move(page);
            return absl::OkStatus();
        }
        AppendPage(&*output, std::move(page));
        return absl::OkStatus();
    });
    if (!status.ok()) {
        return status;
    }
    if (!output.has_value()) {
        return runtime::Value::table_stream("", {});
    }
    runtime::Value value = internal::ValueFromPage(*output);
    value.as_table_mut().materialized = true;
    return value;
}

std::vector<DriverTask> DriverFactory::CreateTasks(size_t pipeline_index, Pipeline pipeline) {
    std::vector<DriverTask> tasks;
    auto make_task = [&](size_t driver_id, std::unique_ptr<Operator> root) {
        Pipeline driver_pipeline;
        driver_pipeline.id = pipeline.id;
        driver_pipeline.name = pipeline.name;
        driver_pipeline.role = pipeline.role;
        driver_pipeline.dependencies = pipeline.dependencies;
        driver_pipeline.operators = pipeline.operators;
        driver_pipeline.distribution = pipeline.distribution;
        driver_pipeline.root = std::move(root);
        driver_pipeline.stats = pipeline.stats;

        DriverTask task;
        task.pipeline_index = pipeline_index;
        task.driver_id = driver_id;
        task.pipeline_id = driver_pipeline.id;
        task.role = driver_pipeline.role;
        task.pipeline = std::move(driver_pipeline);
        tasks.push_back(std::move(task));
    };

    if (!pipeline.driver_roots.empty()) {
        tasks.reserve(pipeline.driver_roots.size());
        for (size_t index = 0; index < pipeline.driver_roots.size(); ++index) {
            make_task(index, std::move(pipeline.driver_roots[index]));
        }
        return tasks;
    }

    make_task(0, std::move(pipeline.root));
    return tasks;
}

ExecutionProfile BuildExecutionProfile(const std::vector<PipelineProfile>& pipeline_templates,
                                       const std::vector<std::shared_ptr<Pipeline::Stats>>& stats,
                                       const std::shared_ptr<QueryMemoryContext>& memory_context) {
    ExecutionProfile profile;
    if (memory_context != nullptr) {
        profile.memory = memory_context->Snapshot();
    }
    profile.pipelines = pipeline_templates;
    for (size_t index = 0; index < profile.pipelines.size() && index < stats.size(); ++index) {
        if (stats[index] == nullptr) {
            continue;
        }
        std::scoped_lock lock(stats[index]->mu);
        profile.pipelines[index].pages = stats[index]->pages;
        profile.pipelines[index].rows = stats[index]->rows;
        profile.pipelines[index].split_stats = stats[index]->split_stats;
        profile.pipelines[index].accumulator_stats = stats[index]->accumulator_stats;
        profile.pipelines[index].exchange_partition_stats = stats[index]->exchange_partition_stats;
        profile.pipelines[index].blocked = stats[index]->blocked;
        profile.pipelines[index].finished = stats[index]->finished;
        profile.pipelines[index].error = stats[index]->error;
    }
    return profile;
}

bool IsBlockingOperatorName(const std::string& name) {
    return name == "SortOperator" || name == "GroupOperator" || name == "AggregateOperator" ||
           name == "PartialAggregateOperator" || name == "FinalAggregateOperator" ||
           name == "PartialGroupOperator" || name == "FinalGroupOperator" ||
           name == "PartialDistinctOperator" || name == "FinalDistinctOperator" ||
           name == "DistinctOperator" || name == "LocalHashJoinOperator" ||
           name == "ExchangeOperator" || name == "MaterializeOperator" || name == "TopNOperator";
}

bool internal::HasBlockingOperator(const std::vector<std::string>& operators) {
    for (const auto& name : operators) {
        if (IsBlockingOperatorName(name)) {
            return true;
        }
    }
    return false;
}

absl::StatusOr<SchedulerResult> Scheduler::RunWithProfile(ExecutionTask task) const {
    if (task.pipelines.empty()) {
        return absl::InvalidArgumentError("execution task has no pipelines");
    }
    if (task.memory_context == nullptr) {
        task.memory_context = QueryMemoryContext::FromEnvironment();
    }
    std::vector<PipelineProfile> pipeline_profiles;
    std::vector<std::shared_ptr<Pipeline::Stats>> pipeline_stats;
    pipeline_profiles.reserve(task.pipelines.size());
    pipeline_stats.reserve(task.pipelines.size());
    for (const auto& pipeline : task.pipelines) {
        pipeline_profiles.push_back(PipelineProfile{
            .id = pipeline.id,
            .name = pipeline.name,
            .role = pipeline.role,
            .dependencies = pipeline.dependencies,
            .operators = pipeline.operators,
            .distribution = pipeline.distribution,
            .blocking = internal::HasBlockingOperator(pipeline.operators),
            .drivers = pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size(),
            .split_stats = {},
            .accumulator_stats = {},
            .error = {}});
        pipeline_stats.push_back(pipeline.stats);
    }

    std::unordered_map<std::string, size_t> pipeline_indexes;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        if (!task.pipelines[index].id.empty()) {
            pipeline_indexes.emplace(task.pipelines[index].id, index);
        }
    }

    std::vector<size_t> indegree(task.pipelines.size(), 0);
    std::vector<std::vector<size_t>> dependents(task.pipelines.size());
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        for (const auto& dependency : task.pipelines[index].dependencies) {
            auto it = pipeline_indexes.find(dependency);
            if (it == pipeline_indexes.end()) {
                return absl::InvalidArgumentError(
                    absl::StrCat("pipeline depends on missing pipeline: ", dependency));
            }
            ++indegree[index];
            dependents[it->second].push_back(index);
        }
    }
    std::queue<size_t> ready;
    for (size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            ready.push(index);
        }
    }
    size_t visited = 0;
    while (!ready.empty()) {
        const size_t index = ready.front();
        ready.pop();
        ++visited;
        for (size_t dependent : dependents[index]) {
            --indegree[dependent];
            if (indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }
    if (visited != task.pipelines.size()) {
        return absl::FailedPreconditionError("execution task pipeline dependency cycle");
    }

    std::optional<runtime::TableValue> output;
    bool ran_pipeline = false;

    struct RunningPipeline {
        size_t index = 0;
        std::string id;
        std::string role;
        size_t driver_id = 0;
        bool collect_output = false;
        std::future<absl::StatusOr<runtime::TableValue>> value;
    };

    std::vector<DriverTask> driver_tasks;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        ResetPipelineStats(task.pipelines[index].stats);
        if (task.pipelines[index].root == nullptr && task.pipelines[index].driver_roots.empty()) {
            FinishPipelineStats(task.pipelines[index].stats);
            continue;
        }
        auto tasks = DriverFactory::CreateTasks(index, std::move(task.pipelines[index]));
        driver_tasks.insert(driver_tasks.end(),
                            std::make_move_iterator(tasks.begin()),
                            std::make_move_iterator(tasks.end()));
    }

    std::vector<RunningPipeline> running;
    running.reserve(driver_tasks.size());
    TaskExecutor executor(std::max<size_t>(1, driver_tasks.size()));
    for (auto& item : driver_tasks) {
        ran_pipeline = true;
        RunningPipeline running_pipeline;
        running_pipeline.index = item.pipeline_index;
        running_pipeline.id = item.pipeline_id;
        running_pipeline.role = item.role;
        running_pipeline.driver_id = item.driver_id;
        running_pipeline.collect_output =
            item.role == "root" || item.pipeline_id == "main" || task.pipelines.size() == 1;
        const bool collect_output = running_pipeline.collect_output;
        running_pipeline.value = executor.Submit([driver_task = std::move(item),
                                                  collect_output]() mutable
                                                     -> absl::StatusOr<runtime::TableValue> {
            runtime::TableValue table;
            auto status =
                Driver(std::move(driver_task.pipeline)).RunToSink([&](const runtime::Page& page) {
                    if (!collect_output) {
                        return absl::OkStatus();
                    }
                    runtime::TableValue next = TableValueFromPage(page);
                    if (table.bucket.empty()) {
                        table.bucket = next.bucket;
                    }
                    table.tables.insert(table.tables.end(),
                                        std::make_move_iterator(next.tables.begin()),
                                        std::make_move_iterator(next.tables.end()));
                    table.rows.insert(table.rows.end(),
                                      std::make_move_iterator(next.rows.begin()),
                                      std::make_move_iterator(next.rows.end()));
                    table.plan = next.plan;
                    return absl::OkStatus();
                });
            if (!status.ok()) {
                return status;
            }
            return table;
        });
        running.push_back(std::move(running_pipeline));
    }

    std::optional<absl::Status> output_error;
    std::optional<absl::Status> non_output_error;
    for (auto& running_pipeline : running) {
        auto table_or = running_pipeline.value.get();
        if (!table_or.ok()) {
            if (running_pipeline.collect_output) {
                if (!output_error.has_value()) {
                    output_error = table_or.status();
                }
            } else if (!non_output_error.has_value()) {
                non_output_error = table_or.status();
            }
            continue;
        }
        if (!running_pipeline.collect_output) {
            continue;
        }
        if (!output.has_value()) {
            output = std::move(*table_or);
            continue;
        }
        output->tables.insert(output->tables.end(),
                              std::make_move_iterator(table_or->tables.begin()),
                              std::make_move_iterator(table_or->tables.end()));
        output->rows.insert(output->rows.end(),
                            std::make_move_iterator(table_or->rows.begin()),
                            std::make_move_iterator(table_or->rows.end()));
    }
    if (output_error.has_value()) {
        return *output_error;
    }
    if (non_output_error.has_value()) {
        return *non_output_error;
    }
    if (!ran_pipeline) {
        return absl::InvalidArgumentError("execution task has no runnable pipelines");
    }
    if (!output.has_value()) {
        SchedulerResult result;
        result.value = runtime::Value::table_stream("", {});
        result.profile =
            BuildExecutionProfile(pipeline_profiles, pipeline_stats, task.memory_context);
        return result;
    }
    runtime::Value value = runtime::Value::table_stream(output->bucket,
                                                        output->tables,
                                                        output->range_start,
                                                        output->range_stop,
                                                        output->result_name);
    value.as_table_mut().plan = output->plan;
    value.as_table_mut().materialized = true;
    SchedulerResult result;
    result.value = std::move(value);
    result.profile = BuildExecutionProfile(pipeline_profiles, pipeline_stats, task.memory_context);
    return result;
}

absl::StatusOr<runtime::Value> Scheduler::Run(ExecutionTask task) const {
    auto result_or = RunWithProfile(std::move(task));
    if (!result_or.ok()) {
        return result_or.status();
    }
    return std::move(result_or->value);
}

absl::StatusOr<SchedulerStreamResult> Scheduler::RunToSink(ExecutionTask task,
                                                           const PageSink& sink) const {
    if (task.pipelines.empty()) {
        return absl::InvalidArgumentError("execution task has no pipelines");
    }
    if (task.memory_context == nullptr) {
        task.memory_context = QueryMemoryContext::FromEnvironment();
    }
    if (sink == nullptr) {
        return absl::InvalidArgumentError("scheduler sink is missing");
    }

    std::vector<PipelineProfile> pipeline_profiles;
    std::vector<std::shared_ptr<Pipeline::Stats>> pipeline_stats;
    pipeline_profiles.reserve(task.pipelines.size());
    pipeline_stats.reserve(task.pipelines.size());
    for (const auto& pipeline : task.pipelines) {
        pipeline_profiles.push_back(PipelineProfile{
            .id = pipeline.id,
            .name = pipeline.name,
            .role = pipeline.role,
            .dependencies = pipeline.dependencies,
            .operators = pipeline.operators,
            .distribution = pipeline.distribution,
            .blocking = internal::HasBlockingOperator(pipeline.operators),
            .drivers = pipeline.driver_roots.empty() ? 1 : pipeline.driver_roots.size(),
            .split_stats = {},
            .accumulator_stats = {},
            .error = {}});
        pipeline_stats.push_back(pipeline.stats);
    }

    std::unordered_map<std::string, size_t> pipeline_indexes;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        if (!task.pipelines[index].id.empty()) {
            pipeline_indexes.emplace(task.pipelines[index].id, index);
        }
    }
    std::vector<size_t> indegree(task.pipelines.size(), 0);
    std::vector<std::vector<size_t>> dependents(task.pipelines.size());
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        for (const auto& dependency : task.pipelines[index].dependencies) {
            auto it = pipeline_indexes.find(dependency);
            if (it == pipeline_indexes.end()) {
                return absl::InvalidArgumentError(
                    absl::StrCat("pipeline depends on missing pipeline: ", dependency));
            }
            ++indegree[index];
            dependents[it->second].push_back(index);
        }
    }
    std::queue<size_t> ready;
    for (size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            ready.push(index);
        }
    }
    size_t visited = 0;
    while (!ready.empty()) {
        const size_t index = ready.front();
        ready.pop();
        ++visited;
        for (size_t dependent : dependents[index]) {
            --indegree[dependent];
            if (indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }
    if (visited != task.pipelines.size()) {
        return absl::FailedPreconditionError("execution task pipeline dependency cycle");
    }

    std::vector<DriverTask> driver_tasks;
    for (size_t index = 0; index < task.pipelines.size(); ++index) {
        ResetPipelineStats(task.pipelines[index].stats);
        if (task.pipelines[index].root == nullptr && task.pipelines[index].driver_roots.empty()) {
            FinishPipelineStats(task.pipelines[index].stats);
            continue;
        }
        auto tasks = DriverFactory::CreateTasks(index, std::move(task.pipelines[index]));
        driver_tasks.insert(driver_tasks.end(),
                            std::make_move_iterator(tasks.begin()),
                            std::make_move_iterator(tasks.end()));
    }
    if (driver_tasks.empty()) {
        return absl::InvalidArgumentError("execution task has no runnable pipelines");
    }

    struct RunningPipeline {
        bool stream_output = false;
        std::future<absl::Status> status;
    };
    std::mutex sink_mu;
    std::vector<RunningPipeline> running;
    running.reserve(driver_tasks.size());
    TaskExecutor executor(std::max<size_t>(1, driver_tasks.size()));
    for (auto& item : driver_tasks) {
        RunningPipeline running_pipeline;
        running_pipeline.stream_output =
            item.role == "root" || item.pipeline_id == "main" || task.pipelines.size() == 1;
        const bool stream_output = running_pipeline.stream_output;
        running_pipeline.status = executor.Submit(
            [driver_task = std::move(item), stream_output, &sink, &sink_mu]() mutable {
                return Driver(std::move(driver_task.pipeline)).RunToSink([&](runtime::Page page) {
                    if (!stream_output) {
                        return absl::OkStatus();
                    }
                    std::scoped_lock lock(sink_mu);
                    return sink(std::move(page));
                });
            });
        running.push_back(std::move(running_pipeline));
    }
    std::optional<absl::Status> output_error;
    std::optional<absl::Status> non_output_error;
    for (auto& running_pipeline : running) {
        auto status = running_pipeline.status.get();
        if (!status.ok()) {
            if (running_pipeline.stream_output) {
                if (!output_error.has_value()) {
                    output_error = status;
                }
            } else if (!non_output_error.has_value()) {
                non_output_error = status;
            }
        }
    }
    if (output_error.has_value()) {
        return *output_error;
    }
    if (non_output_error.has_value()) {
        return *non_output_error;
    }
    SchedulerStreamResult result;
    result.profile = BuildExecutionProfile(pipeline_profiles, pipeline_stats, task.memory_context);
    return result;
}

} // namespace pl::flux::execution
