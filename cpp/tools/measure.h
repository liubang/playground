//=====================================================================
//
// tools.h -
//
// Created by liubang on 2023/06/18 00:32
// Last Modified: 2023/06/18 00:32
//
//=====================================================================
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
    std::cout << "[" << name << "]"
              << " >> Elapsed time: " << elapsed.count() << "(us)" << std::endl;
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
    std::cout << "[" << name << "]"
              << " >> Elapsed time: " << elapsed.count() << "(us)" << std::endl;

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
        std::cout << "[" << name << "]"
                  << " >> Elapsed time: " << elapsed.count() << "(us)" << std::endl;
    } else {
        auto ret = fn();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "[" << name << "]"
                  << " >> Elapsed time: " << elapsed.count() << "(us)" << std::endl;
        return ret;
    }
}

#endif

} // namespace pl
