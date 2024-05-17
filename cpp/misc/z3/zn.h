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

#include "types.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace pl::curve {

template <int32_t Dims, int32_t BitsPerDim, int32_t TotalBits, uint64_t MaxMask> class ZN {
public:
    virtual ~ZN() = default;

    /**
     * @brief Insert (Dims - 1) zeros between each bit to create a zvalue from a single dimension.
     * Only the first BitsPerDim can be considered
     *
     * @param value value to split
     * @return
     */
    [[nodiscard]] virtual uint64_t split(uint64_t value) const = 0;

    /**
     * @brief Conbine every (Dims - 1) bits to re-create a single dimension. Opposite of split.
     *
     * @param z value to combine
     * @return
     */
    [[nodiscard]] virtual uint64_t combine(uint64_t z) const = 0;

    /**
     * @brief Is the value contained in the range. Considers user-space
     *
     * @param range
     * @param value
     * @return
     */
    [[nodiscard]] virtual bool contains(const Zrange& range, uint64_t value) const = 0;

    /**
     * @brief Is the value contained in the range. Considers user-space
     *
     * @param range
     * @param value
     * @return
     */
    [[nodiscard]] bool contains(const Zrange& range, const Zrange& value) const {
        return contains(range, value.min) && contains(range, value.max);
    }

    /**
     * @brief Does the value overlap with the range. Considers user-space
     *
     * @param range
     * @param value
     * @return
     */
    [[nodiscard]] virtual bool overlaps(const Zrange& range, const Zrange& value) const = 0;

    /**
     * @brief Returns (litmax, bigmin) for the given range and point
     *
     * @param p point
     * @param rmin min value
     * @param rmax max value
     * @return (litmax, bigmin)
     */
    [[nodiscard]] std::pair<uint64_t, uint64_t> zdivide(uint64_t p,
                                                        uint64_t rmin,
                                                        uint64_t rmax) const {
        // TODO:
        return {0, 0};
    }

    void zranges(const std::vector<Zrange>& zbounds,
                 int32_t precision,
                 int32_t max_ranges,
                 int32_t max_recurse,
                 std::vector<IndexRange>* idx_ranges) {
        // TODO:
    }

protected:
    using LoadFunc = std::function<uint64_t(uint64_t, uint64_t, uint32_t, uint32_t)>;

    [[nodiscard]] std::pair<uint64_t, uint64_t> zdiv(
        LoadFunc&& load, uint32_t dims, uint64_t xd, uint64_t rmin, uint64_t rmax) const {
        assert(rmin < rmax);
        // TODO:
        auto zmin = rmin;
        auto zmax = rmax;
    }

protected:
    static constexpr int32_t DEFAULT_RECURSE = 7;
    static constexpr int32_t DIMS = Dims;
    static constexpr int32_t BITS_PER_DIM = BitsPerDim;
    static constexpr int32_t TOTAL_BITS = TotalBits;
    static constexpr uint64_t MAX_MASK = MaxMask;
    static constexpr double QUADRANTS = std::pow(2, Dims);
};

} // namespace pl::curve
