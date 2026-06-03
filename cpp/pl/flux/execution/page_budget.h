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

#include <algorithm>
#include <cstddef>

#include "cpp/pl/flux/runtime/runtime_page.h"

namespace pl::flux::execution {

inline size_t EstimateValueBytes(const runtime::Value& value) {
    switch (value.type()) {
        case runtime::Value::Type::String:
            return value.as_string().size();
        case runtime::Value::Type::Time:
            return value.as_time().literal.size();
        case runtime::Value::Type::Array:
        case runtime::Value::Type::Object:
        case runtime::Value::Type::Function:
        case runtime::Value::Type::Table:
            return value.string().size();
        default:
            return sizeof(runtime::Value);
    }
}

inline size_t EstimatePageBytes(const runtime::Page& page) {
    size_t bytes = sizeof(runtime::Page);
    for (const auto& chunk : page.chunks) {
        bytes += sizeof(runtime::PageChunk);
        bytes += chunk.columns.size() * sizeof(runtime::ColumnVector);
        for (const auto& column : chunk.columns) {
            bytes += column.name.size();
            bytes += column.values.capacity() * sizeof(runtime::Value);
            for (const auto& value : column.values) {
                bytes += EstimateValueBytes(value);
            }
        }
    }
    return std::max<size_t>(bytes, 1);
}

} // namespace pl::flux::execution
