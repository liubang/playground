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

#include <iostream>
#include <ostream>
#include <tuple>

namespace pl {

template <typename... Args> void PrintImpl(const Args&... tupleArgs) {
    std::size_t index = 0;
    auto print_elem = [&index](const auto& x) {
        if (index++ > 0) {
            std::cout << ", ";
        }
        std::cout << x;
    };

    (print_elem(tupleArgs), ...);
}

template <typename... Args> void PrintTupleApplyFn(const std::tuple<Args...> tp) {
    std::cout << "(";
    std::apply(PrintImpl<Args...>, tp);
    std::cout << ")";
}

template <typename TupleT> void PrintTupleApply(const TupleT& tp) {
    std::cout << "(";
    std::apply(
        [](const auto&... tupleArgs) {
            std::size_t index = 0;
            auto print_elem = [&index](const auto& x) {
                if (index++ > 0) {
                    std::cout << ", ";
                }
                std::cout << x;
            };
            (print_elem(tupleArgs), ...);
        },
        tp);
    std::cout << ")";
}

} // namespace pl
