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
// Created: 2026/04/24 22:10

#include "cpp/pl/flux/runtime_builtin_package.h"

#include <unordered_map>
#include <utility>

namespace pl::flux::builtin {
namespace {

std::unordered_map<std::string, PackageBuilder>& package_builders() {
    static auto* builders = new std::unordered_map<std::string, PackageBuilder>();
    return *builders;
}

} // namespace

void RegisterPackage(const std::string& path, PackageBuilder builder) {
    package_builders()[path] = std::move(builder);
}

std::optional<Value> ImportRegisteredPackage(const std::string& path) {
    const auto& builders = package_builders();
    auto it = builders.find(path);
    if (it == builders.end()) {
        return std::nullopt;
    }
    return it->second();
}

Value MakeUnknownPackage(const std::string& path) {
    return Value::object({{"path", Value::string(path)}});
}

} // namespace pl::flux::builtin
