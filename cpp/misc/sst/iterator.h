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

#include <functional>
#include <list>

#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"

namespace pl {

class Iterator {
public:
    Iterator();
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    virtual ~Iterator();

    virtual void first() = 0;

    virtual void last() = 0;

    virtual void next() = 0;

    virtual void prev() = 0;

    virtual void seek(const Binary& target) = 0;

    [[nodiscard]] virtual Status status() const = 0;

    [[nodiscard]] virtual bool valid() const = 0;

    [[nodiscard]] virtual Binary key() const = 0;

    [[nodiscard]] virtual Binary val() const = 0;

    using CleanupFunc = std::function<void()>;
    void registerCleanup(const CleanupFunc& function);

private:
    std::list<CleanupFunc> cleanup_funcs_;
};

} // namespace pl
