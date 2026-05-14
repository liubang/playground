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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstddef>
#include <cstdint>
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

struct PageColumnSchema {
    std::string name;
    Value::Type type = Value::Type::Null;
    bool nullable = false;
};

struct PageSchema {
    std::vector<PageColumnSchema> columns;

    [[nodiscard]] std::optional<size_t> FindColumn(const std::string& name) const {
        for (size_t index = 0; index < columns.size(); ++index) {
            if (columns[index].name == name) {
                return index;
            }
        }
        return std::nullopt;
    }
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

inline Value::Type merge_page_type(Value::Type current, const Value& value) {
    if (value.is_null()) {
        return current;
    }
    if (current == Value::Type::Null) {
        return value.type();
    }
    return current;
}

inline PageSchema SchemaFromPageChunk(const PageChunk& chunk) {
    PageSchema schema;
    schema.columns.reserve(chunk.columns.size());
    for (const auto& column : chunk.columns) {
        PageColumnSchema item;
        item.name = column.name;
        item.type = column.type;
        item.nullable = false;
        for (const auto& value : column.values) {
            item.type = merge_page_type(item.type, value);
            if (value.is_null()) {
                item.nullable = true;
            }
        }
        schema.columns.push_back(std::move(item));
    }
    return schema;
}

inline PageSchema SchemaFromPage(const Page& page) {
    PageSchema schema;
    for (const auto& chunk : page.chunks) {
        PageSchema chunk_schema = SchemaFromPageChunk(chunk);
        for (const auto& column : chunk_schema.columns) {
            auto index = schema.FindColumn(column.name);
            if (!index.has_value()) {
                schema.columns.push_back(column);
                continue;
            }
            auto& existing = schema.columns[*index];
            if (existing.type == Value::Type::Null) {
                existing.type = column.type;
            }
            existing.nullable = existing.nullable || column.nullable;
        }
    }
    return schema;
}

inline absl::Status ValidatePageChunk(const PageChunk& chunk) {
    for (const auto& column : chunk.columns) {
        if (column.values.size() != chunk.row_count) {
            return absl::InvalidArgumentError(absl::StrCat(
                "page column ", column.name, " has ", column.values.size(),
                " values but chunk has ", chunk.row_count, " rows"));
        }
    }
    return absl::OkStatus();
}

inline absl::Status ValidatePage(const Page& page) {
    for (const auto& chunk : page.chunks) {
        auto status = ValidatePageChunk(chunk);
        if (!status.ok()) {
            return status;
        }
    }
    return absl::OkStatus();
}

inline const Value* PageChunkValueAt(const PageChunk& chunk,
                                     size_t row_index,
                                     const std::string& column_name) {
    if (row_index >= chunk.row_count) {
        return nullptr;
    }
    for (const auto& column : chunk.columns) {
        if (column.name != column_name) {
            continue;
        }
        if (row_index >= column.values.size()) {
            return nullptr;
        }
        return &column.values[row_index];
    }
    return nullptr;
}

inline std::shared_ptr<ObjectValue> RowFromPageChunk(const PageChunk& chunk, size_t row_index) {
    std::vector<std::pair<std::string, Value>> props;
    props.reserve(chunk.columns.size());
    if (row_index >= chunk.row_count) {
        return std::make_shared<ObjectValue>(std::move(props));
    }
    for (const auto& column : chunk.columns) {
        props.emplace_back(column.name,
                           row_index < column.values.size() ? column.values[row_index]
                                                            : Value::null());
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

inline PageChunk SlicePageChunkRows(const PageChunk& source, size_t start, size_t count) {
    PageChunk chunk;
    chunk.group_key = source.group_key;
    if (start >= source.row_count || count == 0) {
        chunk.row_count = 0;
        chunk.columns.reserve(source.columns.size());
        for (const auto& column : source.columns) {
            chunk.columns.push_back(ColumnVector{
                .name = column.name,
                .type = column.type,
            });
        }
        return chunk;
    }

    const size_t end = std::min(source.row_count, start + count);
    chunk.row_count = end - start;
    chunk.columns.reserve(source.columns.size());
    for (const auto& source_column : source.columns) {
        ColumnVector column;
        column.name = source_column.name;
        column.type = source_column.type;
        column.values.reserve(chunk.row_count);
        column.values.insert(column.values.end(),
                             source_column.values.begin() + static_cast<std::ptrdiff_t>(start),
                             source_column.values.begin() + static_cast<std::ptrdiff_t>(end));
        chunk.columns.push_back(std::move(column));
    }
    return chunk;
}

inline Page PageLike(const Page& source, std::vector<PageChunk> chunks) {
    Page page;
    page.bucket = source.bucket;
    page.chunks = std::move(chunks);
    page.range_start = source.range_start;
    page.range_stop = source.range_stop;
    page.result_name = source.result_name;
    page.plan = source.plan;
    page.materialized = source.materialized;
    return page;
}

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
    if (output->bucket.empty()) {
        output->bucket = page.bucket;
    }
    if (!output->range_start.has_value()) {
        output->range_start = page.range_start;
    }
    if (!output->range_stop.has_value()) {
        output->range_stop = page.range_stop;
    }
    if (!output->result_name.has_value()) {
        output->result_name = page.result_name;
    }
    if (output->plan == nullptr) {
        output->plan = page.plan;
    }
    output->materialized = output->materialized || page.materialized;
    output->chunks.insert(output->chunks.end(), page.chunks.begin(), page.chunks.end());
}

} // namespace pl::flux
