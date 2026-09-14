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

#include "cpp/pl/prism/dialect/dialect.h"

namespace pl::prism::dialect {

namespace {
const Dialect kTrino{.name = "trino", .identifier_quote = '"', .ilike = true, .try_cast = true};
const Dialect kSpark{.name = "spark", .identifier_quote = '`', .ilike = false, .try_cast = false};
} // namespace

const Dialect& trino() {
    return kTrino;
}

const Dialect& spark() {
    return kSpark;
}

} // namespace pl::prism::dialect
