// Copyright (c) 2023 The Authors. All rights reserved.
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

#pragma once

#include "absl/status/statusor.h"
#include "cpp/pl/flux/ast.h"
#include "cpp/pl/flux/runtime_env.h"
#include <vector>

namespace pl {

class ExpressionEvaluator {
public:
    static absl::StatusOr<Value> Evaluate(const Expression& expr, const Environment& env);
    static absl::StatusOr<Value> Invoke(const Value& callee, const std::vector<Value>& positional_args);
};

} // namespace pl
