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

#include <type_traits>

namespace pl {
namespace detail {

struct DisableCopy {
    constexpr DisableCopy() noexcept = default;
    ~DisableCopy() noexcept = default;

    // disable copy
    DisableCopy(const DisableCopy&) = delete;
    DisableCopy(DisableCopy&&) = default;
    DisableCopy& operator=(const DisableCopy&) = delete;
    DisableCopy& operator=(DisableCopy&&) = default;
};

struct DisableCopyAndMove {
    constexpr DisableCopyAndMove() noexcept = default;
    ~DisableCopyAndMove() noexcept = default;

    // disable copy and move
    DisableCopyAndMove(const DisableCopyAndMove&) = delete;
    DisableCopyAndMove(DisableCopyAndMove&&) = delete;
    DisableCopyAndMove& operator=(const DisableCopyAndMove&) = delete;
    DisableCopyAndMove& operator=(DisableCopyAndMove&&) = delete;
};

struct Default {};

template <bool Copy, bool Move>
using EnableCopyMove =
    std::conditional_t<Copy, Default, std::conditional_t<Move, DisableCopy, DisableCopyAndMove>>;

} // namespace detail

using detail::DisableCopy;
using detail::DisableCopyAndMove;

} // namespace pl
