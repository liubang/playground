#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <vector>

#include "basecode/cistring.h"

TEST(cistring, compare)
{
    std::vector<std::pair<const char*, const char*>> cases;

    cases = {
        {"", ""},
        {"A", "A"},
        {"A", "a"},
        {"ab", "AB"},
        {"aB", "Ab"},
    };

    for (const auto& [a, b] : cases) {
        const basecode::cistring s1(a);
        const basecode::cistring s2(b);

        EXPECT_TRUE(s1 == s1);
        EXPECT_TRUE(s2 == s2);
        EXPECT_TRUE(s1 == s2);
        EXPECT_TRUE(s2 == s1);
        EXPECT_TRUE(s1 >= s2);
        EXPECT_TRUE(s1 <= s2);
        EXPECT_FALSE(s1 > s2);
        EXPECT_FALSE(s2 > s1);
    }

    cases = {
        {"a", "b"},
        {"a", "B"},
        {"A", "b"},
        {"A", "B"},
        {"aa", "bb"},
        {"Aa", "Bb"},
        {"AA", "BB"},
    };

    for (const auto& [a, b] : cases) {
        const basecode::cistring s1(a);
        const basecode::cistring s2(b);

        EXPECT_TRUE(s1 < s2);
        EXPECT_TRUE(s2 > s1);
    }
}

TEST(cistring, find)
{
    const basecode::cistring s1("heLLo WorLd");
    EXPECT_EQ(0u, s1.find("h"));
    EXPECT_EQ(0u, s1.find("H"));
    EXPECT_EQ(1u, s1.find("e"));
    EXPECT_EQ(1u, s1.find("E"));
    EXPECT_EQ(2u, s1.find("l"));
    EXPECT_EQ(9u, s1.rfind("l"));
    EXPECT_EQ(9u, s1.rfind("L"));
    EXPECT_EQ(-1, s1.rfind("B"));
    EXPECT_EQ(-1, s1.rfind("b"));
}
