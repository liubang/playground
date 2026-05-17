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
// Created: 2026/05/17 18:27

#include "cpp/pl/flux/execution/accumulator.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pl::flux::execution {
namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* kPartialAggregateCountColumn = "_flux_acc_count";
constexpr size_t kDefaultAccumulatorMemoryLimitBytes = size_t{256} * 1024 * 1024;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class ScopedAccumulatorTimer {
public:
    explicit ScopedAccumulatorTimer(double* target) : target_(target), start_(Clock::now()) {}
    ~ScopedAccumulatorTimer() {
        if (target_ != nullptr) {
            *target_ += elapsed_ms(start_);
        }
    }

private:
    double* target_ = nullptr;
    Clock::time_point start_;
};

absl::StatusOr<std::optional<Page>> next_accumulator_input_page(Operator* input) {
    if (input == nullptr) {
        return absl::InvalidArgumentError("operator has no input");
    }
    return input->NextPage();
}

size_t parse_accumulator_memory_limit() {
    const char* value = std::getenv("FLUX_ACCUMULATOR_MAX_BYTES");
    if (value == nullptr || *value == '\0') {
        return kDefaultAccumulatorMemoryLimitBytes;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed == 0) {
        return kDefaultAccumulatorMemoryLimitBytes;
    }
    return static_cast<size_t>(parsed);
}

size_t estimate_value_memory_bytes(const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
        case Value::Type::Bool:
        case Value::Type::Int:
        case Value::Type::UInt:
        case Value::Type::Float:
        case Value::Type::Time:
        case Value::Type::Duration:
        case Value::Type::Regex:
            return sizeof(Value);
        case Value::Type::String:
            return sizeof(Value) + value.as_string().size();
        case Value::Type::Array: {
            size_t bytes = sizeof(Value);
            for (const auto& element : value.as_array().elements) {
                bytes += estimate_value_memory_bytes(element);
            }
            return bytes;
        }
        case Value::Type::Object: {
            size_t bytes = sizeof(Value);
            for (const auto& [name, property] : value.as_object().properties) {
                bytes += name.size() + estimate_value_memory_bytes(property);
            }
            return bytes;
        }
        case Value::Type::Table:
        case Value::Type::Function:
            return sizeof(Value);
    }
    return sizeof(Value);
}

size_t estimate_group_key_memory_bytes(const std::shared_ptr<ObjectValue>& group_key) {
    constexpr size_t kStateOverhead = 128;
    if (group_key == nullptr) {
        return kStateOverhead;
    }
    size_t bytes = kStateOverhead;
    for (const auto& [name, value] : group_key->properties) {
        bytes += name.size() + estimate_value_memory_bytes(value);
    }
    return bytes;
}

absl::Status account_accumulator_memory(AccumulatorStats* stats, size_t bytes) {
    if (stats == nullptr || bytes == 0) {
        return absl::OkStatus();
    }
    stats->memory_bytes += bytes;
    if (stats->memory_limit_bytes != 0 && stats->memory_bytes > stats->memory_limit_bytes) {
        stats->memory_limited = true;
        return absl::ResourceExhaustedError(absl::StrCat(
            "accumulator memory limit exceeded: estimated=", stats->memory_bytes,
            " bytes, limit=", stats->memory_limit_bytes, " bytes"));
    }
    return absl::OkStatus();
}

Page page_with_plan(Page page, const std::shared_ptr<plan::PlanNode>& plan) {
    page.plan = plan;
    page.materialized = true;
    return page;
}

bool is_numeric_value(const Value& value) {
    return value.type() == Value::Type::Int || value.type() == Value::Type::UInt ||
           value.type() == Value::Type::Float;
}

double numeric_value(const Value& value) {
    switch (value.type()) {
        case Value::Type::Int:
            return static_cast<double>(value.as_int());
        case Value::Type::UInt:
            return static_cast<double>(value.as_uint());
        case Value::Type::Float:
            return value.as_float();
        default:
            return 0.0;
    }
}

size_t hash_combine(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

size_t value_hash(const Value& value) {
    size_t seed = std::hash<int>{}(static_cast<int>(value.type()));
    switch (value.type()) {
        case Value::Type::Null:
            return seed;
        case Value::Type::Bool:
            return hash_combine(seed, std::hash<bool>{}(value.as_bool()));
        case Value::Type::Int:
            return hash_combine(seed, std::hash<int64_t>{}(value.as_int()));
        case Value::Type::UInt:
            return hash_combine(seed, std::hash<uint64_t>{}(value.as_uint()));
        case Value::Type::Float:
            return hash_combine(seed, std::hash<double>{}(value.as_float()));
        case Value::Type::String:
            return hash_combine(seed, std::hash<std::string>{}(value.as_string()));
        case Value::Type::Duration:
            return hash_combine(seed, std::hash<std::string>{}(value.as_duration().literal));
        case Value::Type::Time:
            return hash_combine(seed, std::hash<std::string>{}(value.as_time().literal));
        case Value::Type::Regex:
            return hash_combine(seed, std::hash<std::string>{}(value.as_regex().literal));
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Table:
        case Value::Type::Function:
            return hash_combine(seed, std::hash<std::string>{}(value.string()));
    }
    return seed;
}

struct GroupKeyPart {
    bool missing = false;
    Value value = Value::null();

    bool operator==(const GroupKeyPart& other) const {
        if (missing || other.missing) {
            return missing == other.missing;
        }
        return value == other.value;
    }
};

struct GroupKey {
    enum class Kind {
        Empty,
        Single,
        Multi,
    };

    Kind kind = Kind::Empty;
    GroupKeyPart single;
    std::vector<GroupKeyPart> parts;

    bool operator==(const GroupKey& other) const {
        if (kind != other.kind) {
            return false;
        }
        switch (kind) {
            case Kind::Empty:
                return true;
            case Kind::Single:
                return single == other.single;
            case Kind::Multi:
                return parts == other.parts;
        }
        return false;
    }
};

struct GroupKeyHash {
    size_t operator()(const GroupKey& key) const {
        size_t seed = std::hash<int>{}(static_cast<int>(key.kind));
        auto hash_part = [](size_t current, const GroupKeyPart& part) {
            current = hash_combine(current, std::hash<bool>{}(part.missing));
            if (!part.missing) {
                current = hash_combine(current, value_hash(part.value));
            }
            return current;
        };
        switch (key.kind) {
            case GroupKey::Kind::Empty:
                return seed;
            case GroupKey::Kind::Single:
                return hash_part(seed, key.single);
            case GroupKey::Kind::Multi:
                seed = hash_combine(seed, key.parts.size());
                for (const auto& part : key.parts) {
                    seed = hash_part(seed, part);
                }
                return seed;
        }
        return seed;
    }
};

const char* aggregate_phase_name(AggregatePhase phase) {
    switch (phase) {
        case AggregatePhase::Single:
            return "single";
        case AggregatePhase::Partial:
            return "partial";
        case AggregatePhase::Final:
            return "final";
    }
    return "single";
}

struct PageMetadata {
    std::string bucket;
    std::optional<std::string> range_start;
    std::optional<std::string> range_stop;
    std::optional<std::string> result_name;
    bool initialized = false;
};

struct AggregateState {
    std::shared_ptr<ObjectValue> group_key;
    int64_t count = 0;
    size_t numeric_count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    bool has_numeric = false;
};

void capture_page_metadata(PageMetadata* metadata, const Page& page) {
    if (metadata == nullptr || metadata->initialized) {
        return;
    }
    metadata->bucket = page.bucket;
    metadata->range_start = page.range_start;
    metadata->range_stop = page.range_stop;
    metadata->result_name = page.result_name;
    metadata->initialized = true;
}

Page page_from_accumulated_page_chunks(PageMetadata metadata,
                                       std::vector<PageChunk> chunks,
                                       const std::shared_ptr<plan::PlanNode>& plan) {
    Page output;
    output.bucket = std::move(metadata.bucket);
    output.chunks = std::move(chunks);
    output.range_start = std::move(metadata.range_start);
    output.range_stop = std::move(metadata.range_stop);
    output.result_name = std::move(metadata.result_name);
    output.materialized = false;
    return page_with_plan(std::move(output), plan);
}

std::optional<size_t> find_page_chunk_column(const PageChunk& chunk, const std::string& name) {
    return chunk.FindColumn(name);
}

std::vector<std::optional<size_t>> group_column_indexes(const PageChunk& chunk,
                                                        const std::vector<std::string>& columns) {
    std::vector<std::optional<size_t>> indexes;
    indexes.reserve(columns.size());
    for (const auto& column : columns) {
        indexes.push_back(find_page_chunk_column(chunk, column));
    }
    return indexes;
}

const Value* page_chunk_value_at_index(const PageChunk& chunk,
                                       size_t row_index,
                                       std::optional<size_t> column_index) {
    if (!column_index.has_value() || *column_index >= chunk.columns.size() ||
        row_index >= chunk.row_count) {
        return nullptr;
    }
    const auto& column = chunk.columns[*column_index];
    if (row_index >= column.values.size()) {
        return nullptr;
    }
    return &column.values[row_index];
}

GroupKey group_key_for_row(const PageChunk& chunk,
                           size_t row_index,
                           const std::vector<std::optional<size_t>>& column_indexes) {
    GroupKey key;
    if (column_indexes.empty()) {
        key.kind = GroupKey::Kind::Empty;
        return key;
    }
    if (column_indexes.size() == 1) {
        key.kind = GroupKey::Kind::Single;
        const Value* value = page_chunk_value_at_index(chunk, row_index, column_indexes[0]);
        key.single = value == nullptr ? GroupKeyPart{.missing = true}
                                      : GroupKeyPart{.missing = false, .value = *value};
        return key;
    }
    key.kind = GroupKey::Kind::Multi;
    key.parts.reserve(column_indexes.size());
    for (const auto& column_index : column_indexes) {
        const Value* value = page_chunk_value_at_index(chunk, row_index, column_index);
        key.parts.push_back(value == nullptr ? GroupKeyPart{.missing = true}
                                             : GroupKeyPart{.missing = false, .value = *value});
    }
    return key;
}

std::shared_ptr<ObjectValue> group_key_object_for_row(
    const PageChunk& chunk,
    size_t row_index,
    const std::vector<std::string>& columns,
    const std::vector<std::optional<size_t>>& column_indexes) {
    std::vector<std::pair<std::string, Value>> group_props;
    group_props.reserve(columns.size());
    for (size_t index = 0; index < columns.size(); ++index) {
        const Value* value = page_chunk_value_at_index(chunk, row_index, column_indexes[index]);
        if (value != nullptr) {
            group_props.emplace_back(columns[index], *value);
        }
    }
    return std::make_shared<ObjectValue>(std::move(group_props));
}

GroupKey group_key_from_object(const std::shared_ptr<ObjectValue>& object) {
    GroupKey key;
    if (object == nullptr || object->properties.empty()) {
        key.kind = GroupKey::Kind::Empty;
        return key;
    }
    if (object->properties.size() == 1) {
        key.kind = GroupKey::Kind::Single;
        key.single = GroupKeyPart{.missing = false, .value = object->properties[0].second};
        return key;
    }
    key.kind = GroupKey::Kind::Multi;
    key.parts.reserve(object->properties.size());
    for (const auto& [_, value] : object->properties) {
        key.parts.push_back(GroupKeyPart{.missing = false, .value = value});
    }
    return key;
}

ColumnVector make_empty_column_like(const ColumnVector& source, size_t row_count) {
    ColumnVector column;
    column.name = source.name;
    column.type = source.type;
    column.values.resize(row_count, Value::null());
    return column;
}

ColumnVector make_empty_column(std::string name, Value::Type type, size_t row_count) {
    ColumnVector column;
    column.name = std::move(name);
    column.type = type;
    column.values.resize(row_count, Value::null());
    return column;
}

void ensure_column(PageChunk* target, const ColumnVector& source) {
    if (target == nullptr || find_page_chunk_column(*target, source.name).has_value()) {
        return;
    }
    target->columns.push_back(make_empty_column_like(source, target->row_count));
}

void append_source_row_to_chunk(PageChunk* target,
                                const PageChunk& source,
                                size_t row_index,
                                const std::shared_ptr<ObjectValue>& group_key) {
    if (target == nullptr) {
        return;
    }
    for (const auto& source_column : source.columns) {
        if (source_column.name != "_group") {
            ensure_column(target, source_column);
        }
    }
    if (group_key != nullptr) {
        ColumnVector group_column =
            make_empty_column("_group", Value::Type::Object, target->row_count);
        ensure_column(target, group_column);
    }

    for (auto& target_column : target->columns) {
        if (target_column.name == "_group") {
            target_column.values.push_back(Value::object(group_key));
            continue;
        }
        const auto source_index = find_page_chunk_column(source, target_column.name);
        const Value* value = page_chunk_value_at_index(source, row_index, source_index);
        target_column.values.push_back(value == nullptr ? Value::null() : *value);
        if (target_column.type == Value::Type::Null && value != nullptr && !value->is_null()) {
            target_column.type = value->type();
        }
    }
    ++target->row_count;
}

void append_result_column(PageChunk* chunk, std::string name, Value value) {
    if (chunk == nullptr) {
        return;
    }
    ColumnVector column;
    column.name = std::move(name);
    column.type = value.type();
    column.values.push_back(std::move(value));
    chunk->columns.push_back(std::move(column));
}

PageChunk aggregate_result_chunk(const std::shared_ptr<ObjectValue>& group_key,
                                 const std::string& column_name,
                                 std::optional<Value> aggregate_value,
                                 std::optional<int64_t> partial_count = std::nullopt) {
    PageChunk chunk;
    chunk.group_key = group_key;
    if (!aggregate_value.has_value()) {
        chunk.row_count = 0;
        return chunk;
    }
    chunk.row_count = 1;
    if (group_key != nullptr) {
        for (const auto& [name, value] : group_key->properties) {
            append_result_column(&chunk, name, value);
        }
        append_result_column(&chunk, "_group",
                             Value::object(std::make_shared<ObjectValue>(*group_key)));
    }
    append_result_column(&chunk, column_name, std::move(*aggregate_value));
    if (partial_count.has_value()) {
        append_result_column(&chunk, kPartialAggregateCountColumn, Value::integer(*partial_count));
    }
    return chunk;
}

absl::Status update_aggregate_state(AggregateState* state,
                                    const plan::AggregateSpec& aggregate,
                                    const Value* value,
                                    const Value* partial_count = nullptr,
                                    AggregatePhase phase = AggregatePhase::Single) {
    if (state == nullptr) {
        return absl::InvalidArgumentError("aggregate state is missing");
    }
    if (aggregate.fn == plan::AggregateFunction::Count) {
        if (phase == AggregatePhase::Final) {
            if (value == nullptr || value->is_null()) {
                return absl::OkStatus();
            }
            if (value->type() == Value::Type::Int) {
                state->count += value->as_int();
                return absl::OkStatus();
            }
            if (value->type() == Value::Type::UInt) {
                state->count += static_cast<int64_t>(value->as_uint());
                return absl::OkStatus();
            }
            return absl::InvalidArgumentError(
                absl::StrCat("partial count `", aggregate.column, "` must be integral"));
        }
        if (value != nullptr) {
            ++state->count;
        }
        return absl::OkStatus();
    }
    if (value == nullptr || value->is_null()) {
        return absl::OkStatus();
    }
    if (aggregate.fn == plan::AggregateFunction::Mean && phase == AggregatePhase::Final) {
        if (!is_numeric_value(*value)) {
            return absl::InvalidArgumentError(
                absl::StrCat("partial mean sum `", aggregate.column, "` must be numeric"));
        }
        if (partial_count == nullptr || partial_count->is_null()) {
            return absl::OkStatus();
        }
        int64_t count = 0;
        if (partial_count->type() == Value::Type::Int) {
            count = partial_count->as_int();
        } else if (partial_count->type() == Value::Type::UInt) {
            count = static_cast<int64_t>(partial_count->as_uint());
        } else {
            return absl::InvalidArgumentError("partial mean count must be integral");
        }
        if (count <= 0) {
            return absl::OkStatus();
        }
        const double item = numeric_value(*value);
        state->sum += item;
        state->numeric_count += static_cast<size_t>(count);
        if (!state->has_numeric) {
            state->min = item;
            state->max = item;
            state->has_numeric = true;
        } else {
            state->min = std::min(state->min, item);
            state->max = std::max(state->max, item);
        }
        return absl::OkStatus();
    }
    if (!is_numeric_value(*value)) {
        return absl::InvalidArgumentError(
            absl::StrCat("aggregate `", aggregate.column, "` must be numeric"));
    }
    const double item = numeric_value(*value);
    state->sum += item;
    if (!state->has_numeric) {
        state->min = item;
        state->max = item;
        state->has_numeric = true;
    } else {
        state->min = std::min(state->min, item);
        state->max = std::max(state->max, item);
    }
    ++state->numeric_count;
    return absl::OkStatus();
}

absl::Status update_aggregate_state_from_column(AggregateState* state,
                                                const plan::AggregateSpec& aggregate,
                                                const ColumnVector* column) {
    if (aggregate.fn == plan::AggregateFunction::Count) {
        if (state != nullptr && column != nullptr) {
            state->count += static_cast<int64_t>(column->values.size());
        }
        return absl::OkStatus();
    }
    if (column == nullptr) {
        return absl::OkStatus();
    }
    for (const auto& value : column->values) {
        auto status = update_aggregate_state(state, aggregate, &value);
        if (!status.ok()) {
            return status;
        }
    }
    return absl::OkStatus();
}

std::optional<Value> aggregate_value_from_state(const AggregateState& state,
                                                const plan::AggregateSpec& aggregate,
                                                AggregatePhase phase = AggregatePhase::Single) {
    if (aggregate.fn == plan::AggregateFunction::Count) {
        return Value::integer(state.count);
    }
    if (!state.has_numeric) {
        return std::nullopt;
    }
    double value = 0.0;
    switch (aggregate.fn) {
        case plan::AggregateFunction::Sum:
            value = state.sum;
            break;
        case plan::AggregateFunction::Mean:
            value = phase == AggregatePhase::Partial
                        ? state.sum
                        : state.sum / static_cast<double>(state.numeric_count);
            break;
        case plan::AggregateFunction::Min:
            value = state.min;
            break;
        case plan::AggregateFunction::Max:
            value = state.max;
            break;
        case plan::AggregateFunction::Count:
            break;
    }
    return Value::floating(value);
}

void collect_child_split_stats(const std::unique_ptr<Operator>& input,
                               std::vector<connector::ConnectorSplitStats>* out) {
    if (input != nullptr) {
        input->CollectSplitStats(out);
    }
}

void collect_child_accumulator_stats(const std::unique_ptr<Operator>& input,
                                     const AccumulatorStats& stats,
                                     std::vector<AccumulatorStats>* out) {
    if (out == nullptr) {
        return;
    }
    if (input != nullptr) {
        input->CollectAccumulatorStats(out);
    }
    if (stats.input_rows != 0 || stats.output_rows != 0 || stats.groups != 0 ||
        stats.memory_bytes != 0 || stats.memory_limited) {
        out->push_back(stats);
    }
}

} // namespace

StreamingGroupOperator::StreamingGroupOperator(std::shared_ptr<plan::PlanNode> plan,
                                               std::unique_ptr<Operator> input)
    : plan_(std::move(plan)), input_(std::move(input)) {
    stats_.operator_name = name();
    stats_.mode = "group";
    stats_.phase = "single";
    stats_.key_strategy =
        plan_ != nullptr && plan_->group().columns.size() == 1 ? "single" : "generic";
    stats_.memory_limit_bytes = parse_accumulator_memory_limit();
}

std::string StreamingGroupOperator::name() const { return "GroupOperator"; }

void StreamingGroupOperator::Cancel() {
    if (input_ != nullptr) {
        input_->Cancel();
    }
}

void StreamingGroupOperator::CollectSplitStats(
    std::vector<connector::ConnectorSplitStats>* out) const {
    collect_child_split_stats(input_, out);
}

void StreamingGroupOperator::CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const {
    collect_child_accumulator_stats(input_, stats_, out);
}

absl::StatusOr<std::optional<Page>> StreamingGroupOperator::NextPage() {
    if (emitted_) {
        return std::nullopt;
    }
    emitted_ = true;

    PageMetadata metadata;
    std::vector<PageChunk> chunks;
    std::unordered_map<GroupKey, size_t, GroupKeyHash> chunk_indexes;
    while (true) {
        auto page_or = next_accumulator_input_page(input_.get());
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        const Page& page = **page_or;
        auto status = ValidatePage(page);
        if (!status.ok()) {
            return status;
        }
        capture_page_metadata(&metadata, page);
        for (const auto& page_chunk : page.chunks) {
            const auto group_indexes = group_column_indexes(page_chunk, plan_->group().columns);
            for (size_t row_index = 0; row_index < page_chunk.row_count; ++row_index) {
                ++stats_.input_rows;
                GroupKey group_key;
                {
                    ScopedAccumulatorTimer timer(&stats_.key_time_ms);
                    group_key = group_key_for_row(page_chunk, row_index, group_indexes);
                }
                std::pair<std::unordered_map<GroupKey, size_t, GroupKeyHash>::iterator, bool>
                    insertion;
                {
                    ScopedAccumulatorTimer timer(&stats_.hash_time_ms);
                    insertion = chunk_indexes.emplace(std::move(group_key), chunks.size());
                }
                if (insertion.second) {
                    chunks.emplace_back();
                    chunks.back().group_key = group_key_object_for_row(
                        page_chunk, row_index, plan_->group().columns, group_indexes);
                    auto memory_status = account_accumulator_memory(
                        &stats_, estimate_group_key_memory_bytes(chunks.back().group_key));
                    if (!memory_status.ok()) {
                        return memory_status;
                    }
                }
                {
                    ScopedAccumulatorTimer timer(&stats_.update_time_ms);
                    append_source_row_to_chunk(&chunks[insertion.first->second], page_chunk,
                                               row_index,
                                               chunks[insertion.first->second].group_key);
                }
            }
        }
    }
    stats_.groups = chunks.size();
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    stats_.output_rows = 0;
    for (const auto& chunk : chunks) {
        stats_.output_rows += chunk.row_count;
    }
    ScopedAccumulatorTimer timer(&stats_.result_time_ms);
    return page_from_accumulated_page_chunks(std::move(metadata), std::move(chunks), plan_);
}

StreamingDistinctOperator::StreamingDistinctOperator(std::shared_ptr<plan::PlanNode> plan,
                                                     std::unique_ptr<Operator> input)
    : plan_(std::move(plan)), input_(std::move(input)) {
    stats_.operator_name = name();
    stats_.mode = "distinct";
    stats_.phase = "single";
    stats_.key_strategy = "single";
    stats_.memory_limit_bytes = parse_accumulator_memory_limit();
}

std::string StreamingDistinctOperator::name() const { return "DistinctOperator"; }

void StreamingDistinctOperator::Cancel() {
    if (input_ != nullptr) {
        input_->Cancel();
    }
}

void StreamingDistinctOperator::CollectSplitStats(
    std::vector<connector::ConnectorSplitStats>* out) const {
    collect_child_split_stats(input_, out);
}

void StreamingDistinctOperator::CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const {
    collect_child_accumulator_stats(input_, stats_, out);
}

absl::StatusOr<std::optional<Page>> StreamingDistinctOperator::NextPage() {
    if (emitted_) {
        return std::nullopt;
    }
    emitted_ = true;

    struct DistinctState {
        PageChunk chunk;
        std::unordered_set<GroupKey, GroupKeyHash> seen;
    };

    PageMetadata metadata;
    std::vector<DistinctState> groups;
    std::unordered_map<GroupKey, size_t, GroupKeyHash> group_indexes;
    while (true) {
        auto page_or = next_accumulator_input_page(input_.get());
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        const Page& page = **page_or;
        auto status = ValidatePage(page);
        if (!status.ok()) {
            return status;
        }
        capture_page_metadata(&metadata, page);
        for (const auto& page_chunk : page.chunks) {
            GroupKey group_key = group_key_from_object(page_chunk.group_key);
            auto [it, inserted] = group_indexes.emplace(group_key, groups.size());
            if (inserted) {
                DistinctState state;
                state.chunk.group_key = page_chunk.group_key;
                groups.push_back(std::move(state));
                auto memory_status = account_accumulator_memory(
                    &stats_, estimate_group_key_memory_bytes(groups.back().chunk.group_key));
                if (!memory_status.ok()) {
                    return memory_status;
                }
            }
            DistinctState& state = groups[it->second];
            const auto distinct_column =
                find_page_chunk_column(page_chunk, plan_->distinct().column);
            for (size_t row_index = 0; row_index < page_chunk.row_count; ++row_index) {
                ++stats_.input_rows;
                GroupKey value_key;
                size_t value_key_memory_bytes = 0;
                {
                    ScopedAccumulatorTimer timer(&stats_.key_time_ms);
                    const Value* value =
                        page_chunk_value_at_index(page_chunk, row_index, distinct_column);
                    value_key.kind = GroupKey::Kind::Single;
                    value_key.single = value == nullptr
                                           ? GroupKeyPart{.missing = true}
                                           : GroupKeyPart{.missing = false, .value = *value};
                    value_key_memory_bytes =
                        value == nullptr ? sizeof(Value) : estimate_value_memory_bytes(*value);
                }
                bool keep = false;
                {
                    ScopedAccumulatorTimer timer(&stats_.hash_time_ms);
                    keep = state.seen.insert(std::move(value_key)).second;
                }
                if (keep) {
                    auto memory_status =
                        account_accumulator_memory(&stats_, value_key_memory_bytes);
                    if (!memory_status.ok()) {
                        return memory_status;
                    }
                    ScopedAccumulatorTimer timer(&stats_.update_time_ms);
                    append_source_row_to_chunk(&state.chunk, page_chunk, row_index,
                                               page_chunk.group_key);
                }
            }
        }
    }

    std::vector<PageChunk> chunks;
    chunks.reserve(groups.size());
    for (auto& group : groups) {
        chunks.push_back(std::move(group.chunk));
    }
    stats_.groups = chunks.size();
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    stats_.output_rows = 0;
    for (const auto& chunk : chunks) {
        stats_.output_rows += chunk.row_count;
    }
    ScopedAccumulatorTimer timer(&stats_.result_time_ms);
    return page_from_accumulated_page_chunks(std::move(metadata), std::move(chunks), plan_);
}

StreamingAggregateOperator::StreamingAggregateOperator(std::shared_ptr<plan::PlanNode> plan,
                                                       std::unique_ptr<Operator> input)
    : plan_(std::move(plan)), input_(std::move(input)) {
    stats_.operator_name = name();
    stats_.mode = "aggregate";
    stats_.phase = "single";
    stats_.key_strategy = "group_object";
    stats_.memory_limit_bytes = parse_accumulator_memory_limit();
}

std::string StreamingAggregateOperator::name() const { return "AggregateOperator"; }

void StreamingAggregateOperator::Cancel() {
    if (input_ != nullptr) {
        input_->Cancel();
    }
}

void StreamingAggregateOperator::CollectSplitStats(
    std::vector<connector::ConnectorSplitStats>* out) const {
    collect_child_split_stats(input_, out);
}

void StreamingAggregateOperator::CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const {
    collect_child_accumulator_stats(input_, stats_, out);
}

absl::StatusOr<std::optional<Page>> StreamingAggregateOperator::NextPage() {
    if (emitted_) {
        return std::nullopt;
    }
    emitted_ = true;

    PageMetadata metadata;
    std::vector<AggregateState> groups;
    std::unordered_map<GroupKey, size_t, GroupKeyHash> group_indexes;
    while (true) {
        auto page_or = next_accumulator_input_page(input_.get());
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        const Page& page = **page_or;
        auto status = ValidatePage(page);
        if (!status.ok()) {
            return status;
        }
        capture_page_metadata(&metadata, page);
        for (const auto& page_chunk : page.chunks) {
            stats_.input_rows += page_chunk.row_count;
            GroupKey group_key = group_key_from_object(page_chunk.group_key);
            auto [it, inserted] = group_indexes.emplace(std::move(group_key), groups.size());
            if (inserted) {
                AggregateState state;
                state.group_key = page_chunk.group_key;
                groups.push_back(std::move(state));
                auto memory_status = account_accumulator_memory(
                    &stats_, estimate_group_key_memory_bytes(groups.back().group_key));
                if (!memory_status.ok()) {
                    return memory_status;
                }
            }
            AggregateState& state = groups[it->second];
            const auto aggregate_column_index =
                find_page_chunk_column(page_chunk, plan_->aggregate().column);
            const ColumnVector* aggregate_column =
                aggregate_column_index.has_value() ? &page_chunk.columns[*aggregate_column_index]
                                                   : nullptr;
            ScopedAccumulatorTimer timer(&stats_.update_time_ms);
            auto update_status =
                update_aggregate_state_from_column(&state, plan_->aggregate(), aggregate_column);
            if (!update_status.ok()) {
                return update_status;
            }
        }
    }

    std::vector<PageChunk> chunks;
    chunks.reserve(groups.empty() ? 1 : groups.size());
    if (groups.empty()) {
        groups.emplace_back();
    }
    {
        ScopedAccumulatorTimer timer(&stats_.result_time_ms);
        for (const auto& state : groups) {
            chunks.push_back(
                aggregate_result_chunk(state.group_key, plan_->aggregate().column,
                                       aggregate_value_from_state(state, plan_->aggregate())));
        }
    }
    stats_.groups = groups.size();
    stats_.output_rows = 0;
    for (const auto& chunk : chunks) {
        stats_.output_rows += chunk.row_count;
    }
    return page_from_accumulated_page_chunks(std::move(metadata), std::move(chunks), plan_);
}

StreamingGroupedAggregateOperator::StreamingGroupedAggregateOperator(
    std::shared_ptr<plan::PlanNode> aggregate_plan,
    std::vector<std::string> group_columns,
    std::unique_ptr<Operator> input,
    AggregatePhase phase)
    : aggregate_plan_(std::move(aggregate_plan)),
      group_columns_(std::move(group_columns)),
      input_(std::move(input)),
      phase_(phase) {
    stats_.operator_name = name();
    stats_.mode = "grouped_aggregate";
    stats_.phase = aggregate_phase_name(phase_);
    stats_.key_strategy = group_columns_.size() == 1 ? "single" : "generic";
    stats_.memory_limit_bytes = parse_accumulator_memory_limit();
}

std::string StreamingGroupedAggregateOperator::name() const { return "AggregateOperator"; }

void StreamingGroupedAggregateOperator::Cancel() {
    if (input_ != nullptr) {
        input_->Cancel();
    }
}

void StreamingGroupedAggregateOperator::CollectSplitStats(
    std::vector<connector::ConnectorSplitStats>* out) const {
    collect_child_split_stats(input_, out);
}

void StreamingGroupedAggregateOperator::CollectAccumulatorStats(
    std::vector<AccumulatorStats>* out) const {
    collect_child_accumulator_stats(input_, stats_, out);
}

absl::StatusOr<std::optional<Page>> StreamingGroupedAggregateOperator::NextPage() {
    if (emitted_) {
        return std::nullopt;
    }
    emitted_ = true;

    PageMetadata metadata;
    std::vector<AggregateState> groups;
    std::unordered_map<GroupKey, size_t, GroupKeyHash> group_indexes;
    while (true) {
        auto page_or = next_accumulator_input_page(input_.get());
        if (!page_or.ok()) {
            return page_or.status();
        }
        if (!page_or->has_value()) {
            break;
        }
        const Page& page = **page_or;
        auto status = ValidatePage(page);
        if (!status.ok()) {
            return status;
        }
        capture_page_metadata(&metadata, page);
        for (const auto& page_chunk : page.chunks) {
            const auto group_indexes_in_chunk = group_column_indexes(page_chunk, group_columns_);
            const auto aggregate_column =
                find_page_chunk_column(page_chunk, aggregate_plan_->aggregate().column);
            const auto partial_count_column =
                find_page_chunk_column(page_chunk, kPartialAggregateCountColumn);
            for (size_t row_index = 0; row_index < page_chunk.row_count; ++row_index) {
                ++stats_.input_rows;
                GroupKey group_key;
                {
                    ScopedAccumulatorTimer timer(&stats_.key_time_ms);
                    group_key = group_key_for_row(page_chunk, row_index, group_indexes_in_chunk);
                }
                std::pair<std::unordered_map<GroupKey, size_t, GroupKeyHash>::iterator, bool>
                    insertion;
                {
                    ScopedAccumulatorTimer timer(&stats_.hash_time_ms);
                    insertion = group_indexes.emplace(std::move(group_key), groups.size());
                }
                if (insertion.second) {
                    AggregateState state;
                    state.group_key = group_key_object_for_row(
                        page_chunk, row_index, group_columns_, group_indexes_in_chunk);
                    groups.push_back(std::move(state));
                    auto memory_status = account_accumulator_memory(
                        &stats_, estimate_group_key_memory_bytes(groups.back().group_key));
                    if (!memory_status.ok()) {
                        return memory_status;
                    }
                }
                AggregateState& state = groups[insertion.first->second];
                {
                    ScopedAccumulatorTimer timer(&stats_.update_time_ms);
                    auto update_status = update_aggregate_state(
                        &state, aggregate_plan_->aggregate(),
                        page_chunk_value_at_index(page_chunk, row_index, aggregate_column),
                        page_chunk_value_at_index(page_chunk, row_index, partial_count_column),
                        phase_);
                    if (!update_status.ok()) {
                        return update_status;
                    }
                }
            }
        }
    }

    if (groups.empty()) {
        if (!group_columns_.empty()) {
            stats_.groups = 0;
            stats_.output_rows = 0;
            return page_from_accumulated_page_chunks(std::move(metadata), {}, aggregate_plan_);
        }
        AggregateState state;
        groups.push_back(std::move(state));
    }
    std::vector<PageChunk> chunks;
    chunks.reserve(groups.size());
    {
        ScopedAccumulatorTimer timer(&stats_.result_time_ms);
        for (const auto& state : groups) {
            std::optional<int64_t> partial_count;
            if (phase_ == AggregatePhase::Partial &&
                aggregate_plan_->aggregate().fn == plan::AggregateFunction::Mean) {
                partial_count = static_cast<int64_t>(state.numeric_count);
            }
            chunks.push_back(aggregate_result_chunk(
                state.group_key, aggregate_plan_->aggregate().column,
                aggregate_value_from_state(state, aggregate_plan_->aggregate(), phase_),
                partial_count));
        }
    }
    stats_.groups = groups.size();
    stats_.output_rows = 0;
    for (const auto& chunk : chunks) {
        stats_.output_rows += chunk.row_count;
    }
    return page_from_accumulated_page_chunks(std::move(metadata), std::move(chunks),
                                             aggregate_plan_);
}

} // namespace pl::flux::execution
