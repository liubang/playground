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
// Created: 2026/09/14 13:44

#pragma once

#include <string>

#include "cpp/pl/prism/dialect/dialect.h"
#include "cpp/pl/prism/syntax/ast.h"

namespace pl::prism::printer {

// Prints an AST back to canonical SQL text. The output is deterministic and
// re-parseable: for the trino dialect, parse(print(parse(x))) yields an AST
// equivalent to parse(x) — this round-trip property is verified over the
// whole golden corpus.
std::string print(const syntax::Node* node, const dialect::Dialect& dialect = dialect::trino());

} // namespace pl::prism::printer
