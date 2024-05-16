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

#include <cassert>
#include <cstdint>

namespace pl::curve {

struct Box {
    double xmin;
    double ymin;
    double xmax;
    double ymax;
};

struct TimeRange {
    uint64_t tmin;
    uint64_t tmax;
};

struct IndexRange {
    uint64_t lower;
    uint64_t upper;
    bool contained;
};

struct Zrange {
    uint64_t min;
    uint64_t max;

    Zrange(uint64_t min, uint64_t max) : min(min), max(max) { assert(min <= max); }

    [[nodiscard]] uint32_t length() const { return max - min; }

    [[nodiscard]] bool contains(uint64_t bits) const { return bits >= min && bits <= max; }

    [[nodiscard]] bool contains(const Zrange& r) const {
        return contains(r.min) && contains(r.max);
    }

    [[nodiscard]] bool overlap(const Zrange& r) const { return contains(r.min) || contains(r.max); }
};

} // namespace pl::curve
