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

#include <bitset>
#include <catch2/catch_test_macros.hpp>
#include <numeric>
#include <string>
#include <vector>

// since c++20
TEST_CASE("modern", "[test_accumulate]") {
    std::vector<int> nums = {1, 2, 3};
    auto sum = std::accumulate(nums.begin(), nums.end(), 0);
    REQUIRE(6 == sum);

    struct Person {
        std::string name;
        int age;
    };

    std::vector<Person> persons = {{"zhangsan", 10}, {"lisi", 15}, {"wangwu", 11}};

    sum = std::accumulate(persons.begin(), persons.end(), 0, [](auto res, const auto& person) {
        return res + person.age;
    });

    REQUIRE((10 + 15 + 11) == sum);

    std::vector<std::string> strs = {"a", "b", "c", "d"};
    auto joined =
        std::accumulate(strs.begin() + 1, strs.end(), strs[0], [](auto res, const auto& str) {
            return res + "-" + str;
        });

    REQUIRE("a-b-c-d" == joined);

    std::vector<int> a = {1, 2, 3, 4};
    auto b = std::accumulate(a.begin(), a.end(), 0, [](auto res, const auto& i) {
        return res |= (1 << i);
    });

    auto bs = std::bitset<8>(b).to_string();
    REQUIRE("00011110" == bs);
}
