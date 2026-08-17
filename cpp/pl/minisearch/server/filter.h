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
// Created: 2026/08/17

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "cpp/pl/minisearch/core/document.h"
#include "cpp/pl/minisearch/core/schema.h"
#include "cpp/pl/minisearch/proto/minisearch.pb.h"

namespace pl::minisearch::server {

// Filter 编译（DESIGN.md §7.3，M1 后置过滤）。
//
// 子句语义：
//   - field 必须存在于 schema；子句间取 AND；
//   - op: = != > >= < <= in；= / != / 范围操作取 values[0]，in 取全部；
//   - text/keyword 字段匹配 string 值，numeric 字段匹配 double 值，
//     vector 字段不可过滤；
//   - 文档缺该字段时：!= 为 true，其余 op 为 false。

// 校验过滤表达式，返回第一个错误的描述（nullopt 表示合法）。
std::optional<std::string> ValidateFilter(const proto::Filter& filter, const core::Schema& schema);

// 编译为文档谓词。调用前必须先通过 ValidateFilter。
std::function<bool(const core::Document&)> BuildFilterPredicate(const proto::Filter& filter);

} // namespace pl::minisearch::server
