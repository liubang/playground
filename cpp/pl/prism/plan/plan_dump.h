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

#include "cpp/pl/prism/plan/plan.h"

namespace pl::prism::plan {

// Renders a logical plan as a compact single-line s-expression, e.g.
//   SELECT a FROM t WHERE a > 0
//     -> (project (col a) (filter (> a 0) (scan t)))
// Embedded expressions use syntax::dump. Intended for tests and debugging.
std::string dump(const PlanNode* node);

} // namespace pl::prism::plan
