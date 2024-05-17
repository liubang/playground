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

#include "zn.h"

#include <bitset>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <tuple>

namespace pl::curve {

class Z3 : public ZN<3, 21, 64, 0x1FFFFFUL> {
public:
    // the bits will be encoded in reverse order:
    // ......z1y1x1z0y0x0
    Z3(uint64_t x, uint64_t y, uint64_t z) { zval_ = (split(x) | split(y) << 1 | split(z) << 2); }

    // constructed by z3 value
    Z3(uint64_t z) { zval_ = z; }

    ~Z3() override = default;

    [[nodiscard]] std::tuple<uint64_t, uint64_t, uint64_t> decode() const {
        return {combine(zval_), combine(zval_ >> 1), combine(zval_ >> 2)};
    }

    [[nodiscard]] uint64_t val() const { return zval_; }

    friend std::ostream& operator<<(std::ostream& os, const Z3& z3) {
        std::bitset<64> bs(z3.zval_);
        os << bs << '\n';
        return os;
    }

private:
    // insert 00 between every bit in value. Only first 21 bits can be considered.
    [[nodiscard]] uint64_t split(uint64_t val) const override {
        uint64_t x = val & MAX_MASK;
        x = (x | x << 32) & 0x1F00000000FFFFULL;
        x = (x | x << 16) & 0x1F0000FF0000FFULL;
        x = (x | x << 8) & 0x100F00F00F00F00FULL;
        x = (x | x << 4) & 0x10C30C30C30C30C3ULL;
        x = (x | x << 2) & 0x1249249249249249ULL;
        return x;
    }

    // combine every third bit to from a value. Max value is 21 bits
    [[nodiscard]] uint64_t combine(uint64_t z) const override {
        uint64_t x = z & 0x1249249249249249ULL;
        x = (x ^ (x >> 2)) & 0x10C30C30C30C30C3ULL;
        x = (x ^ (x >> 4)) & 0x100F00F00F00F00FULL;
        x = (x ^ (x >> 8)) & 0x1F0000FF0000FFULL;
        x = (x ^ (x >> 16)) & 0x1F00000000FFFFULL;
        x = (x ^ (x >> 32)) & MAX_MASK;
        return x;
    }

    [[nodiscard]] bool contains(const Zrange& range, uint64_t value) const override {
        auto [vx, vy, vz] = Z3(value).decode();
        auto [minx, miny, minz] = Z3(range.min).decode();
        auto [maxx, maxy, maxz] = Z3(range.min).decode();
        return vx >= minx && vx <= maxx && vy >= miny && vy <= maxy && vz >= minz && vz <= maxz;
    }

    [[nodiscard]] bool overlaps(const Zrange& range, const Zrange& value) const override {
        auto [minrx, minry, minrz] = Z3(range.min).decode();
        auto [maxrx, maxry, maxrz] = Z3(range.min).decode();

        auto [minvx, minvy, minvz] = Z3(value.min).decode();
        auto [maxvx, maxvy, maxvz] = Z3(value.min).decode();

        return overlaps(minrx, maxrx, minvx, maxvx) && overlaps(minry, maxry, minvy, maxvy) &&
               overlaps(minrz, maxrz, minvz, maxvz);
    }

    [[nodiscard]] bool overlaps(int64_t a1, int64_t a2, int64_t b1, int64_t b2) const {
        return std::max(a1, b1) <= std::min(a2, b2);
    }

private:
    uint64_t zval_;
};

} // namespace pl::curve
