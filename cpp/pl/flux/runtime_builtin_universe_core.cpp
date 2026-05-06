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
// Created: 2026/04/25 10:40

#include "absl/status/status.h"
#include "cpp/pl/flux/runtime_builtin_universe.h"
#include "cpp/pl/flux/runtime_value.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux {
namespace {

Value make_builtin_value(const std::string& name,
                         FunctionValue::BuiltinCallback fn,
                         std::string pipe_param_name = {}) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->pipe_param_name = std::move(pipe_param_name);
    callable->builtin = std::move(fn);
    return Value::function(std::move(callable));
}

void install_builtin(Environment& env,
                     const std::string& name,
                     FunctionValue::BuiltinCallback fn,
                     std::string pipe_param_name = {}) {
    env.define(name, make_builtin_value(name, std::move(fn), std::move(pipe_param_name)));
}

absl::StatusOr<Value> builtin_len(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("len expects exactly one argument");
    }
    switch (args[0].type()) {
        case Value::Type::String:
            return Value::integer(static_cast<int64_t>(args[0].as_string().size()));
        case Value::Type::Array:
            return Value::integer(static_cast<int64_t>(args[0].as_array().elements.size()));
        case Value::Type::Object:
            return Value::integer(static_cast<int64_t>(args[0].as_object().properties.size()));
        default:
            return absl::InvalidArgumentError("len expects string, array, or object");
    }
}

absl::StatusOr<Value> builtin_string(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("string expects exactly one argument");
    }
    return Value::string(args[0].type() == Value::Type::String ? args[0].as_string()
                                                               : args[0].string());
}

absl::StatusOr<Value> builtin_contains(const std::vector<Value>& args) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            "contains expects a single object argument with `set` and `value`");
    }
    const auto& object = args[0].as_object();
    const Value* set = object.lookup("set");
    const Value* value = object.lookup("value");
    if (set == nullptr || value == nullptr) {
        return absl::InvalidArgumentError("contains requires `set` and `value`");
    }
    if (set->type() != Value::Type::Array) {
        return absl::InvalidArgumentError("contains `set` must be an array");
    }
    for (const auto& item : set->as_array().elements) {
        if (item == *value) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

} // namespace

bool InstallKnownUniverseCoreBuiltin(Environment& env, const std::string& name) {
    if (name == "len") {
        install_builtin(env, "len", builtin_len);
        return true;
    }
    if (name == "string") {
        install_builtin(env, "string", builtin_string);
        return true;
    }
    if (name == "contains") {
        install_builtin(env, "contains", builtin_contains, "set");
        return true;
    }
    return false;
}

void InstallUniverseCoreBuiltins(Environment& env) {
    InstallKnownUniverseCoreBuiltin(env, "len");
    InstallKnownUniverseCoreBuiltin(env, "string");
    InstallKnownUniverseCoreBuiltin(env, "contains");
}

} // namespace pl::flux
