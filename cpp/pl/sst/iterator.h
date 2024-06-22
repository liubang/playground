// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/status/status.h"

#include <memory>

namespace pl {

class Iterator;

using IteratorPtr = std::unique_ptr<Iterator>;

class Iterator {
public:
    Iterator() = default;
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    virtual ~Iterator() = default;

    virtual void first() = 0;

    virtual void last() = 0;

    virtual void next() = 0;

    virtual void prev() = 0;

    virtual void seek(std::string_view target) = 0;

    [[nodiscard]] virtual Status status() const = 0;

    [[nodiscard]] virtual bool valid() const = 0;

    [[nodiscard]] virtual std::string_view key() const = 0;

    [[nodiscard]] virtual std::string_view val() const = 0;
};

} // namespace pl
