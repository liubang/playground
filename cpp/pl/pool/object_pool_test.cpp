// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/pool/object_pool.h"
#include <gtest/gtest.h>

namespace pl::test {

class ObjectPoolTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ObjectPoolTest, Normal) {
    constexpr auto X = 123;
    static size_t destructTimes = 0;
    struct A {
        ~A() { ++destructTimes; }
        int x = X;
    };
    A* rawPtr = nullptr;
    {
        auto aptr = ObjectPool<A>::get();
        EXPECT_NE(nullptr, aptr);
        EXPECT_EQ(X, aptr->x);
        aptr->x = 0;
        rawPtr = aptr.get();
        EXPECT_EQ(0, destructTimes);
        aptr.reset();
        EXPECT_EQ(1, destructTimes);
    }
    EXPECT_EQ(1, destructTimes);
    EXPECT_EQ(rawPtr, ObjectPool<A>::get().get());
    EXPECT_EQ(X, ObjectPool<A>::get()->x);
}

TEST_F(ObjectPoolTest, Allocate) {
    constexpr auto N = 10000000;
    static size_t constructTimes = 0;
    static size_t destructTimes = 0;

    struct A {
        A() { ++constructTimes; }
        ~A() { ++destructTimes; }
    };

    {
        std::vector<ObjectPool<A>::Ptr> vec{N};
        for (auto& item : vec) {
            item = ObjectPool<A>::get();
        }
        EXPECT_EQ(N, constructTimes);
        EXPECT_EQ(0, destructTimes);
    }

    EXPECT_EQ(N, destructTimes);
}

TEST_F(ObjectPoolTest, AllocateAndRelease) {
    constexpr auto N = 10000000;
    static size_t constructTimes = 0;
    static size_t destructTimes = 0;

    struct A {
        A() { ++constructTimes; }
        ~A() { ++destructTimes; }
    };

    for (auto i = 0; i < N; ++i) {
        ObjectPool<A>::get();
    }
    EXPECT_EQ(N, constructTimes);
    EXPECT_EQ(N, destructTimes);
}

} // namespace pl::test
