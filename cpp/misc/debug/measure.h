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

#include <chrono>
#include <iostream>
#include <type_traits>

namespace pl {

#if __cplusplus == 202002L

template <typename Fn>
concept FuncWithVoidRet =
    std::is_invocable_v<std::decay_t<Fn>> && std::is_void_v<std::invoke_result_t<Fn>>;

template <typename Fn>
concept FuncWithNonVoidRet =
    std::is_invocable_v<std::decay_t<Fn>> && !std::is_void_v<std::invoke_result_t<Fn>>;

template <typename Fn>
    requires FuncWithVoidRet<Fn>
void measure(const std::string& name, Fn&& fn) {
    static_assert(std::is_invocable_v<std::decay_t<Fn>>);
    static_assert(std::is_void_v<std::invoke_result_t<Fn>>);
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "[" << name << "]" << " >> Elapsed time: " << elapsed.count() << "(us)"
              << std::endl;
}

template <typename Fn>
    requires FuncWithNonVoidRet<Fn>
auto measure(const std::string& name, Fn&& fn) {
    static_assert(std::is_invocable_v<std::decay_t<Fn>>);
    static_assert(!std::is_void_v<std::invoke_result_t<Fn>>);
    auto start = std::chrono::high_resolution_clock::now();
    auto res = fn();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "[" << name << "]" << " >> Elapsed time: " << elapsed.count() << "(us)"
              << std::endl;

    return res;
}

#else

//// for c++17

template <typename Fn, std::enable_if_t<std::is_invocable_v<std::decay_t<Fn>>>* = nullptr>
auto measure(const std::string& name, Fn&& fn) {
    auto start = std::chrono::high_resolution_clock::now();
    if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
        fn();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "[" << name << "]" << " >> Elapsed time: " << elapsed.count() << "(us)"
                  << std::endl;
    } else {
        auto ret = fn();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "[" << name << "]" << " >> Elapsed time: " << elapsed.count() << "(us)"
                  << std::endl;
        return ret;
    }
}

#endif

} // namespace pl
