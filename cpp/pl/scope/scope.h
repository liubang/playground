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

#include "cpp/pl/scope/preprocessor.h"

#include <type_traits>
#include <utility>

namespace pl {

template <typename Fn> class ScopeGuard {
public:
    ScopeGuard(Fn&& fn) : fn_(std::move(fn)) {}
    ~ScopeGuard() {
        if (enabled_)
            fn_();
    }

    void dismiss() { enabled_ = false; }

private:
    Fn fn_;
    bool enabled_{true};
};

namespace detail {

enum class ScopeGuardOnExit {};
template <typename Fn>
inline ScopeGuard<std::decay_t<Fn>> operator+(detail::ScopeGuardOnExit, Fn&& fn) {
    return ScopeGuard<Fn>(std::forward<Fn>(fn));
}

} // namespace detail

#define SCOPE_EXIT                                                    \
    auto PL_ANONYMOUS_VARIABLE(__playground_cpp_tools_ScopeGuard__) = \
        pl::detail::ScopeGuardOnExit{} + [&]() noexcept

} // namespace pl
