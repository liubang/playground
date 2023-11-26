//=====================================================================
//
// test_statusor.cpp -
//
// Created by liubang on 2023/11/26 13:08
// Last Modified: 2023/11/26 13:08
//
//=====================================================================

#include "absl/status/statusor.h"

#include <gtest/gtest.h>

namespace pl {

class TestAbseilStatusOr : public ::testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}

    absl::StatusOr<int64_t> test1(int a) {
        if (a == 0) {
            return absl::InvalidArgumentError("invalid argument");
        }
        return a;
    }
};

TEST_F(TestAbseilStatusOr, statusor) {
    auto r = test1(0);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ("invalid argument", r.status().message());
    auto rr = test1(12);
    EXPECT_TRUE(rr.ok());
    EXPECT_EQ(12, *rr);
    EXPECT_EQ(12, rr.value());
}

} // namespace pl
