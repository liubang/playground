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
// Created: 2026/05/16 09:39

#pragma once

#include "cpp/pl/flux/runtime/runtime_page.h"
#include <algorithm>
#include <cstddef>

namespace pl::flux::execution {

inline size_t EstimateValueBytes(const Value& value) {
    switch (value.type()) {
        case Value::Type::String:
            return value.as_string().size();
        case Value::Type::Time:
            return value.as_time().literal.size();
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Function:
        case Value::Type::Table:
            return value.string().size();
        default:
            return sizeof(Value);
    }
}

inline size_t EstimatePageBytes(const Page& page) {
    size_t bytes = sizeof(Page);
    for (const auto& chunk : page.chunks) {
        bytes += sizeof(PageChunk);
        bytes += chunk.columns.size() * sizeof(ColumnVector);
        for (const auto& column : chunk.columns) {
            bytes += column.name.size();
            bytes += column.values.capacity() * sizeof(Value);
            for (const auto& value : column.values) {
                bytes += EstimateValueBytes(value);
            }
        }
    }
    return std::max<size_t>(bytes, 1);
}

} // namespace pl::flux::execution
