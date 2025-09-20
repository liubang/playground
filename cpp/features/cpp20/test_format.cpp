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

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>
#include <string>

#if (defined(__GNUC__) && __GNUC__ >= 13) || (defined(__clang_major__) && __clang_major__ >= 16)
#include <format>
#endif

constexpr std::size_t ROWS = 5;
const std::map<std::string, std::array<double, ROWS>> product_to_orders{
    {"apples", {100, 200, 50.5, 30, 10}},
    {"bananas", {80, 10, 100, 120, 70}},
    {"carrots", {130, 75, 25, 64.5, 128}},
    {"tomatoes", {70, 100, 170, 80, 90}}};

/**
 * @brief print table in cpp17
 */
void print_table_cpp17() {
    // print headers
    for (const auto& [key, val] : product_to_orders) {
        std::cout << std::setw(10) << key;
    }
    std::cout << '\n';

    // print values
    for (std::size_t i = 0; i < ROWS; ++i) {
        for (const auto& [key, val] : product_to_orders) {
            std::cout << std::setw(10) << std::fixed << std::setprecision(2) << val[i];
        }
        std::cout << '\n';
    }
}

//-------------------------------------------------------------------------------------------------

#if (defined(__GNUC__) && __GNUC__ >= 13) || (defined(__clang_major__) && __clang_major__ >= 16)
// calculate max column width
template <typename T> std::size_t max_key_length1(const std::map<std::string, T>& m) {
    std::size_t max_len = 0;
    for (const auto& [key, val] : m) {
        max_len = std::max(key.length(), max_len);
    }
    return max_len;
}

template <typename T> std::size_t max_key_length2(const std::map<std::string, T>& m) {
    auto res = std::ranges::max_element(std::views::keys(m), [](const auto& a, const auto& b) {
        return a.length() < b.length();
    });
    return (*res).length();
}

const auto col_length = max_key_length2(product_to_orders) + 2;

/**
 * @brief print table use std::format in cpp20
 * WARN: GCC13 required
 */
void print_table_cpp20() {
    // headers
    for (const auto& name : std::views::keys(product_to_orders)) {
        std::cout << std::format("{:*>{}}", name, col_length);
    }
    std::cout << '\n';
    // print values
    for (std::size_t i = 0; i < ROWS; ++i) {
        for (const auto& values : std::views::values(product_to_orders)) {
            std::cout << std::format("{:>{}.2f}", values[i], col_length);
        }
        std::cout << '\n';
    }
}
#endif

TEST_CASE("modern", "[test_format]") {
    SECTION("c++17 version") { print_table_cpp17(); };

#if (defined(__GNUC__) && __GNUC__ >= 13) || (defined(__clang_major__) && __clang_major__ >= 16)
    SECTION("c++20 version") { print_table_cpp20(); };
#endif
}
