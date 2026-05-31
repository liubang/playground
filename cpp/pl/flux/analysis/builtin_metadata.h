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
// Created: 2026/05/24 11:04

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pl::flux::analysis {

enum class BuiltinParamKind {
    Required,
    Optional,
    Pipe,
};

enum class BuiltinExportKind {
    Function,
    Value,
};

struct BuiltinParam {
    std::string name;
    std::string type;
    BuiltinParamKind kind = BuiltinParamKind::Required;
    std::optional<std::string> default_value{};
};

struct BuiltinSignature {
    std::string package; // Empty means universe.
    std::string name;
    std::string fq_name;
    std::string return_type;
    std::string summary;
    std::vector<BuiltinParam> params;
    bool implemented = true;
    bool provider = false;
    BuiltinExportKind kind = BuiltinExportKind::Function;
};

const std::vector<BuiltinSignature>& AllBuiltinSignatures();
const BuiltinSignature* FindBuiltinSignature(std::string_view package, std::string_view name);
const BuiltinSignature* FindUniverseBuiltinSignature(std::string_view name);
std::vector<const BuiltinSignature*> BuiltinsForPackage(std::string_view package);
std::vector<std::string> KnownPackages();
bool IsKnownPackage(std::string_view package);
bool IsCallableBuiltin(const BuiltinSignature& sig);

std::string SignatureLabel(const BuiltinSignature& sig);
std::string SignatureDetail(const BuiltinSignature& sig);
std::vector<std::string> CompletionParams(const BuiltinSignature& sig);

} // namespace pl::flux::analysis
