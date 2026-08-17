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

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pl::minisearch::core {

// Field value payload: string for text/keyword, double for numeric,
// vector<float> for vector fields.
using FieldValue = std::variant<std::string, double, std::vector<float>>;

// A stored document (DESIGN.md §4.2). `version` follows last-write-wins:
// a request carrying version > 0 is rejected when it does not exceed the
// stored version; version == 0 lets the server assign one.
struct Document {
    std::string id;
    int64_t version = 0;
    int64_t internal_docid = -1; // assigned by the collection on upsert
    std::map<std::string, FieldValue> fields;
};

} // namespace pl::minisearch::core
