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
// Created: 2026/09/14 15:20

#pragma once

#include <string>
#include <vector>

#include "cpp/pl/arena/arena.h"
#include "cpp/pl/prism/plan/plan.h"

namespace pl::prism::plan {

struct PlanError {
    syntax::SourceLocation location;
    std::string message;
};

struct PlanResult {
    PlanNode* root = nullptr;
    std::vector<PlanError> errors;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// AST -> logical plan: a pure structural transform (no catalog, no types).
//
// A QuerySpecification becomes the chain
//   Project -> Window? -> Filter(having)? -> Aggregation? -> Filter(where)?
//   -> <relation plan of FROM>
// and a Query adds Sort / Limit on top of its body.
//
// The produced plan stays valid as long as the AstToPlan object and the
// source AST (and its source text) are alive.
//
// Not yet supported (reported as errors, never silently dropped): WITH,
// LATERAL, TABLESAMPLE, set-operation CORRESPONDING, and the (expr).* select
// form. Aliases of non-table relations are scoping information and pass
// through without being recorded.
class AstToPlan {
public:
    AstToPlan() = default;

    PlanResult translate(const syntax::Node* query);

private:
    template <typename T, typename... Args> T* make(syntax::SourceLocation loc, Args&&... args) {
        return arena_.template allocate_object<T>(loc, std::forward<Args>(args)...);
    }
    template <typename T> syntax::AstList<T> make_list(const std::vector<T>& items) {
        syntax::AstList<T> out;
        out.size = static_cast<uint32_t>(items.size());
        if (!items.empty()) {
            out.data = static_cast<T*>(arena_.allocate(sizeof(T) * items.size(), alignof(T)));
            std::copy(items.begin(), items.end(), out.data);
        }
        return out;
    }

    void fail(syntax::SourceLocation loc, std::string message);

    PlanNode* translate_query(const syntax::Query* query);
    PlanNode* translate_query_body(const syntax::Node* body);
    PlanNode* translate_query_specification(const syntax::QuerySpecification* spec);
    PlanNode* translate_from(const syntax::AstList<syntax::Relation*>& from);
    PlanNode* translate_relation(const syntax::Relation* relation);

    // Collects the windowed function calls of a select-item expression,
    // without descending into subqueries (they have their own scope).
    static void collect_windowed(syntax::Expression* expr, std::vector<syntax::Expression*>& out);

    Arena arena_;
    std::vector<PlanError> errors_;
};

} // namespace pl::prism::plan
