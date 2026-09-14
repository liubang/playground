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

#include <string_view>

namespace pl::prism::dialect {

// A target SQL dialect profile. The printer consults the profile to render
// constructs that do not translate 1:1 between dialects.
struct Dialect {
    std::string_view name;
    char identifier_quote; // '"' standard, '`' Spark
    bool ilike;            // native ILIKE; otherwise LOWER(x) LIKE LOWER(y)
    bool try_cast;         // native TRY_CAST; otherwise falls back to CAST
};

const Dialect& trino();
const Dialect& spark();

} // namespace pl::prism::dialect
