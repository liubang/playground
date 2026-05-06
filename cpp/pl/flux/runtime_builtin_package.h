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

#pragma once

#include "cpp/pl/flux/runtime_value.h"
#include <functional>
#include <optional>
#include <string>

namespace pl::flux::builtin {

using PackageBuilder = std::function<Value()>;

void RegisterPackage(const std::string& path, PackageBuilder builder);
std::optional<Value> ImportRegisteredPackage(const std::string& path);
Value MakeUnknownPackage(const std::string& path);

void RegisterScalarStdlibPackages();
void RegisterTableStdlibPackages();
void RegisterJoinStdlibPackage();
void RegisterSqlStdlibPackage();

} // namespace pl::flux::builtin
