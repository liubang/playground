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
// Created: 2026/05/15 00:31

#pragma once

#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux {

struct ColumnVector {
    std::string name;
    Value::Type type = Value::Type::Null;
    std::vector<Value> values;
};

struct PageChunk {
    std::vector<ColumnVector> columns;
    std::shared_ptr<ObjectValue> group_key;
    size_t row_count = 0;
};

struct Page {
    std::string bucket;
    std::vector<PageChunk> chunks;
    std::optional<std::string> range_start;
    std::optional<std::string> range_stop;
    std::optional<std::string> result_name;
    std::shared_ptr<plan::PlanNode> plan;
    bool materialized = false;

    [[nodiscard]] size_t row_count() const {
        size_t count = 0;
        for (const auto& chunk : chunks) {
            count += chunk.row_count;
        }
        return count;
    }

    [[nodiscard]] bool empty() const { return row_count() == 0; }
};

inline std::vector<std::string> infer_page_columns(const TableChunk& chunk) {
    if (!chunk.columns.empty()) {
        return chunk.columns;
    }
    std::vector<std::string> columns;
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        for (const auto& [key, _] : row->properties) {
            bool seen = false;
            for (const auto& column : columns) {
                if (column == key) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                columns.push_back(key);
            }
        }
    }
    return columns;
}

inline PageChunk PageChunkFromTableChunk(const TableChunk& chunk) {
    PageChunk page_chunk;
    page_chunk.group_key = chunk.group_key;
    page_chunk.row_count = chunk.rows.size();

    const std::vector<std::string> column_names = infer_page_columns(chunk);
    page_chunk.columns.reserve(column_names.size());
    for (const auto& name : column_names) {
        ColumnVector column;
        column.name = name;
        column.values.reserve(chunk.rows.size());
        for (const auto& row : chunk.rows) {
            const Value* value = row == nullptr ? nullptr : row->lookup(name);
            column.values.push_back(value == nullptr ? Value::null() : *value);
            if (column.type == Value::Type::Null && value != nullptr && !value->is_null()) {
                column.type = value->type();
            }
        }
        page_chunk.columns.push_back(std::move(column));
    }
    return page_chunk;
}

inline Page PageFromTableValue(const TableValue& table) {
    Page page;
    page.bucket = table.bucket;
    page.range_start = table.range_start;
    page.range_stop = table.range_stop;
    page.result_name = table.result_name;
    page.plan = table.plan;
    page.materialized = table.materialized;
    page.chunks.reserve(table.tables.size());
    for (const auto& chunk : table.tables) {
        page.chunks.push_back(PageChunkFromTableChunk(chunk));
    }
    return page;
}

inline Page PageFromTableChunks(std::string bucket,
                                const std::vector<TableChunk>& chunks,
                                std::optional<std::string> range_start = std::nullopt,
                                std::optional<std::string> range_stop = std::nullopt,
                                std::optional<std::string> result_name = std::nullopt) {
    Page page;
    page.bucket = std::move(bucket);
    page.range_start = std::move(range_start);
    page.range_stop = std::move(range_stop);
    page.result_name = std::move(result_name);
    page.materialized = false;
    page.chunks.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        page.chunks.push_back(PageChunkFromTableChunk(chunk));
    }
    return page;
}

inline Page PageFromRows(std::string bucket, std::vector<std::shared_ptr<ObjectValue>> rows) {
    TableChunk chunk;
    chunk.rows = std::move(rows);
    std::vector<TableChunk> chunks;
    chunks.push_back(std::move(chunk));
    return PageFromTableChunks(std::move(bucket), chunks);
}

inline TableChunk TableChunkFromPageChunk(const PageChunk& page_chunk) {
    TableChunk chunk;
    chunk.group_key = page_chunk.group_key;
    chunk.columns.reserve(page_chunk.columns.size());
    for (const auto& column : page_chunk.columns) {
        chunk.columns.push_back(column.name);
    }
    chunk.rows.reserve(page_chunk.row_count);
    for (size_t row_index = 0; row_index < page_chunk.row_count; ++row_index) {
        std::vector<std::pair<std::string, Value>> props;
        props.reserve(page_chunk.columns.size());
        for (const auto& column : page_chunk.columns) {
            if (row_index < column.values.size()) {
                props.emplace_back(column.name, column.values[row_index]);
            } else {
                props.emplace_back(column.name, Value::null());
            }
        }
        chunk.rows.push_back(std::make_shared<ObjectValue>(std::move(props)));
    }
    return chunk;
}

inline TableValue TableValueFromPage(const Page& page) {
    std::vector<TableChunk> chunks;
    chunks.reserve(page.chunks.size());
    for (const auto& chunk : page.chunks) {
        chunks.push_back(TableChunkFromPageChunk(chunk));
    }
    Value value = Value::table_stream(page.bucket, std::move(chunks), page.range_start,
                                      page.range_stop, page.result_name);
    value.as_table_mut().plan = page.plan;
    value.as_table_mut().materialized = page.materialized;
    return value.as_table();
}

inline void AppendPage(Page* output, Page page) {
    if (output == nullptr) {
        return;
    }
    output->chunks.insert(output->chunks.end(), page.chunks.begin(), page.chunks.end());
}

} // namespace pl::flux
