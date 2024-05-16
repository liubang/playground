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

#include "z3.h"

#include "z3sfc.h"
#include <cassert>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

std::chrono::system_clock::time_point utc_string_to_time_point(const std::string& utc_string) {
    std::tm tm = {};
    std::istringstream ss(utc_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    std::time_t time = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(time);
}

void debug(pl::curve::Z3 z3) {
    auto [x, y, z] = z3.decode();
    std::cout << "x:" << std::bitset<32>(x) << '\n';
    std::cout << "y:" << std::bitset<32>(y) << '\n';
    std::cout << "z:" << std::bitset<32>(z) << '\n';
}

int main(int argc, char* argv[]) {

    {
        pl::curve::Z3 z3(1, 2, 4);
        std::cout << z3;
        std::cout << std::bitset<64>(z3.split((1ULL << 63) - 1)) << '\n';
    }

    {
        pl::curve::Z3 z3(1, 0, 0);
        std::cout << z3;
        assert(z3.val() == 1);
        auto [x, y, z] = z3.decode();
        assert(x == 1);
        assert(y == 0);
        assert(z == 0);
    }

    {
        pl::curve::Z3 z3(0, 1, 0);
        std::cout << z3;
        assert(z3.val() == 2);
    }

    {
        pl::curve::Z3 z3(0, 0, 1);
        std::cout << z3;
        assert(z3.val() == 4);
    }

    {
        pl::curve::Z3 z3(1, 1, 1);
        std::cout << z3;
        assert(z3.val() == 7);
        auto [x, y, z] = z3.decode();
        assert(x == 1);
        assert(y == 1);
        assert(z == 1);
    }

    {
        pl::curve::Z3SFC<pl::curve::TimePeriod::Week> sfc;
        auto z = sfc.index(45.0, 10.0, 1);
        pl::curve::Z3 z3(z);
        std::cout << z3;
    }

    {
        auto time = utc_string_to_time_point("2024-05-15T12:30:00Z");
        auto binned_time = pl::curve::BinnedTime<pl::curve::TimePeriod::Week>::of(time);
        pl::curve::Z3SFC<pl::curve::TimePeriod::Week> sfc;
        auto z = sfc.index(116.312803, 40.047735, binned_time.offset());
        pl::curve::Z3 z3(z);
        std::cout << z3;
        debug(z3);
    }

    {
        auto time = utc_string_to_time_point("2024-05-15T12:30:00Z");
        // clang-format off
        {
            auto binned_time = pl::curve::BinnedTime<pl::curve::TimePeriod::Year>::of(time);
            std::cout << "[ Year] => bin: " << binned_time.bin() << ", offset: " << binned_time.offset() << '\n';
        }
        {
            auto binned_time = pl::curve::BinnedTime<pl::curve::TimePeriod::Month>::of(time);
            std::cout << "[Month] => bin: " << binned_time.bin() << ", offset: " << binned_time.offset() << '\n';
        }
        {
            auto binned_time = pl::curve::BinnedTime<pl::curve::TimePeriod::Week>::of(time);
            std::cout << "[ Week] => bin: " << binned_time.bin() << ", offset: " << binned_time.offset() << '\n';
        }
        {
            auto binned_time = pl::curve::BinnedTime<pl::curve::TimePeriod::Day>::of(time);
            std::cout << "[  Day] => bin: " << binned_time.bin() << ", offset: " << binned_time.offset() << '\n';
        }

    }

    return 0;
}
