//=====================================================================
//
// test_accumulate.cpp -
//
// Created by liubang on 2023/06/04 16:34
// Last Modified: 2023/06/04 16:34
//
//=====================================================================

#include <bitset>
#include <numeric>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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
