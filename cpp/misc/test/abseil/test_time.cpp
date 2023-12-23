// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include <gtest/gtest.h>

namespace pl {
class TestAbseilTime : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TestAbseilTime, test) {
    absl::Time t1 = absl::Now();
    std::cout << absl::FormatTime(t1, absl::UTCTimeZone()) << "\n";
    absl::Time t2;
    std::string error;
    absl::ParseTime(absl::RFC3339_full, "2023-12-23T10:43:53.887408578Z", &t2, &error);
    std::cout << absl::ToUnixNanos(t2) << '\n';

    std::string formattedTime = absl::FormatTime("%Y-%m-%dT%H:%M:%E9SZ", t2, absl::UTCTimeZone());
    std::cout << formattedTime << std::endl;
}

} // namespace pl
