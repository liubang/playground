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

#include "cpp/pl/minisearch/core/schema.h"

#include <set>

namespace pl::minisearch::core {

namespace {

bool known_metric(const std::string& metric) {
    return metric == "cosine" || metric == "dot_product" || metric == "l2_norm";
}

} // namespace

bool IsValidResourceName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

const FieldDef* Schema::Find(const std::string& name) const {
    auto it = fields.find(name);
    return it == fields.end() ? nullptr : &it->second;
}

std::optional<std::string> Schema::Validate() const {
    std::set<std::string> text_fields;
    for (const auto& [name, def] : fields) {
        if (name.empty()) {
            return "field name must not be empty";
        }
        if (def.name != name) {
            return "field map key and FieldDef.name mismatch: " + name;
        }
        if (def.type == FieldType::kText) {
            text_fields.insert(name);
            continue;
        }
        if (def.type == FieldType::kVector) {
            if (def.dims <= 0 || def.dims > 4096) {
                return "vector field requires 0 < dims <= 4096: " + name;
            }
            if (!known_metric(def.metric)) {
                return "vector field metric must be cosine/dot_product/l2_norm: " + name;
            }
            if (def.indexed) {
                return "vector field must not set indexed (no inverted index): " + name;
            }
            continue;
        }
        // kKeyword / kNumeric.
    }
    for (const auto& [name, def] : fields) {
        if (def.type == FieldType::kVector && def.server_embedded) {
            if (def.source_field.empty()) {
                return "server-embedded vector field requires source_field: " + name;
            }
            if (text_fields.count(def.source_field) == 0) {
                return "source_field must reference a text field: " + def.source_field;
            }
        }
    }
    return std::nullopt;
}

} // namespace pl::minisearch::core
