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

namespace pl::minisearch::core {

// Field types (DESIGN.md §4.1). text/keyword/numeric participate in the
// inverted index or docvalues; vector fields hold float arrays.
enum class FieldType {
    kText,
    kKeyword,
    kNumeric,
    kVector,
};

struct FieldDef {
    std::string name;
    FieldType type = FieldType::kKeyword;

    // text/keyword: term index membership. Ignored (rejected) for vector.
    bool indexed = true;
    // stored in docstore and returned in hits.
    bool stored = true;

    // text only: analyzer override; empty = collection default.
    std::string analyzer;

    // vector only.
    int dims = 0;
    // "cosine" | "dot_product" | "l2_norm".
    std::string metric = "cosine";
    // vector only, mode="server": name of the text field embedded on write.
    // mode="client" leaves this empty and vectors arrive with the document.
    std::string source_field;
    bool server_embedded = false;
};

// Resource names (tenants, collections) must match [A-Za-z0-9_-]{1,64}.
// They are embedded into on-disk paths and the key-store file, so anything
// else (slashes, dots, spaces, CJK) is rejected at the API boundary.
bool IsValidResourceName(const std::string& name);

// Collection schema: field definitions plus collection-level settings.
struct Schema {
    std::map<std::string, FieldDef> fields;

    // Default analyzer for text fields without an override.
    std::string default_analyzer;

    // BM25 parameters (k1, b) and RRF weights are carried in collection
    // settings, not the schema; see DESIGN.md §4.1/§7.2.

    const FieldDef* Find(const std::string& name) const;
    // Returns a description of the first violation, or nullopt when valid.
    // Rules: unique non-empty names; vector requires dims in (0, 4096] and a
    // known metric and forbids `indexed`; `server_embedded` requires
    // `source_field` to name an existing text field.
    std::optional<std::string> Validate() const;
};

} // namespace pl::minisearch::core
