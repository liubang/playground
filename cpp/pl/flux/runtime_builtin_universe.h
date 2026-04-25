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

#pragma once

#include "cpp/pl/flux/runtime_env.h"
#include <string>

namespace pl {

void InstallUniverseCoreBuiltins(Environment& env);
bool InstallKnownUniverseCoreBuiltin(Environment& env, const std::string& name);

void InstallUniverseTransformBuiltins(Environment& env);
bool InstallKnownUniverseTransformBuiltin(Environment& env, const std::string& name);

void InstallUniverseAggregateBuiltins(Environment& env);
bool InstallKnownUniverseAggregateBuiltin(Environment& env, const std::string& name);

void InstallUniverseWindowBuiltins(Environment& env);
bool InstallKnownUniverseWindowBuiltin(Environment& env, const std::string& name);

void InstallUniverseJoinBuiltins(Environment& env);
bool InstallKnownUniverseJoinBuiltin(Environment& env, const std::string& name);

void InstallUniverseInspectBuiltins(Environment& env);
bool InstallKnownUniverseInspectBuiltin(Environment& env, const std::string& name);

} // namespace pl
