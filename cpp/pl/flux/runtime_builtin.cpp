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
// Created: 2026/04/15 00:47

#include "cpp/pl/flux/runtime_builtin.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/runtime_builtin_package.h"
#include "cpp/pl/flux/runtime_builtin_universe.h"

namespace pl {
namespace {

Value make_unimplemented_builtin_value(const std::string& name) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->builtin = [name](const std::vector<Value>&) -> absl::StatusOr<Value> {
        return absl::UnimplementedError(
            absl::StrCat("builtin `", name, "` is declared but not implemented"));
    };
    return Value::function(std::move(callable));
}

bool install_known_builtin(Environment& env, const std::string& name) {
    return InstallKnownUniverseCoreBuiltin(env, name) ||
           InstallKnownUniverseTransformBuiltin(env, name) ||
           InstallKnownUniverseAggregateBuiltin(env, name) ||
           InstallKnownUniverseWindowBuiltin(env, name) ||
           InstallKnownUniverseJoinBuiltin(env, name) ||
           InstallKnownUniverseInspectBuiltin(env, name);
}

} // namespace

void BuiltinRegistry::Install(Environment& env) {
    InstallUniverseCoreBuiltins(env);
    InstallUniverseTransformBuiltins(env);
    InstallUniverseAggregateBuiltins(env);
    InstallUniverseWindowBuiltins(env);
    InstallUniverseJoinBuiltins(env);
    InstallUniverseInspectBuiltins(env);
}

absl::Status BuiltinRegistry::Ensure(Environment& env, const std::string& name) {
    auto current = env.lookup(name);
    if (current.ok()) {
        if (current->type() != Value::Type::Function) {
            return absl::InvalidArgumentError(
                absl::StrCat("builtin name conflicts with non-function binding: ", name));
        }
        return absl::OkStatus();
    }
    if (install_known_builtin(env, name)) {
        return absl::OkStatus();
    }
    env.define(name, make_unimplemented_builtin_value(name));
    return absl::OkStatus();
}

absl::StatusOr<Value> BuiltinRegistry::ImportPackage(const std::string& path) {
    flux_builtin::RegisterTableStdlibPackages();
    flux_builtin::RegisterScalarStdlibPackages();
    flux_builtin::RegisterJoinStdlibPackage();

    auto package = flux_builtin::ImportRegisteredPackage(path);
    if (package.has_value()) {
        return *package;
    }
    return flux_builtin::MakeUnknownPackage(path);
}

} // namespace pl
