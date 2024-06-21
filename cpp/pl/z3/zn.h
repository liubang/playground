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

    /**
     * @brief Calculates ranges in index space that match any of the input bounds.
     * Uses breadth-first searching to allow a limit on the number of ranges returned.
     *
     * @param zbounds search space
     * @param precision precision to consider, in bits (max 64)
     * @param max_ranges loose cap on the number of ranges to return. A higher number of ranges will
     *        have less false positives, but require more processing
     * @param max_recurse max levels of recursion to apply before stopping
     * @param idx_ranges ranges coverting the search space
     */
    void zranges(const std::vector<Zrange>& zbounds,
                 int32_t precision,
                 int32_t max_ranges,
                 int32_t max_recurse,
                 std::vector<IndexRange>* idx_ranges) {
        // TODO:
    }

protected:
    using LoadFunc = std::function<uint64_t(uint64_t, uint64_t, uint32_t, uint32_t)>;

    /**
     * @brief Implements the algorithm defined in: Tropf paper to find:
     *        LITMAX: max z-index in query range smaller than current point, xd
     *        BIGMIN: min z-index in query range greater than current point, xd
     *
     * @param load function that knows how to load bits into appropraite dimension of a z-index
     * @param dims
     * @param xd z-index that is outside of the query range, inclusive
     * @param rmin min z-index of the query range, inclusive
     * @param rmax max z-index of the query range, inclusive
     * @return (LITMAX, BIGMIN)
     */
    [[nodiscard]] std::pair<uint64_t, uint64_t> zdiv(
        LoadFunc&& load, uint32_t dims, uint64_t xd, uint64_t rmin, uint64_t rmax) const {
        assert(rmin < rmax);

        uint64_t zmin = rmin;
        uint64_t zmax = rmax;
        uint64_t bigmin = 0;
        uint64_t litmax = 0;

        auto bit = [](uint64_t x, uint32_t idx) -> uint64_t {
            return (x & (1UL << idx)) >> idx;
        };

        auto over = [](uint64_t bits) -> uint64_t {
            return 1UL << (bits - 1);
        };

        auto under = [](uint64_t bits) -> uint64_t {
            return (1UL << (bits - 1)) - 1;
        };

#define ZN_MATCH_ALL(__x, __y, __z) if ((a == (__x)) && (b == (__y)) && (c == (__z)))

        for (int i = 64; i > 0; --i) {
            uint32_t bits = i / dims + 1;
            uint32_t dim = i % dims;

            uint64_t a = bit(xd, i);
            uint64_t b = bit(zmin, i);
            uint64_t c = bit(zmax, i);

            ZN_MATCH_ALL(0, 0, 0) { continue; }
            ZN_MATCH_ALL(0, 0, 1) {
                zmax = load(zmax, under(bits), bits, dim);
                bigmin = load(zmin, over(bits), bits, dim);
                continue;
            }
            ZN_MATCH_ALL(0, 1, 0) {
                // Not possible, MIN <= MAX
                // assert(false);
                continue;
            }
            ZN_MATCH_ALL(0, 1, 1) {
                bigmin = zmin;
                return {litmax, bigmin};
            }
            ZN_MATCH_ALL(1, 0, 0) {
                litmax = zmax;
                return {litmax, bigmin};
            }
            ZN_MATCH_ALL(1, 0, 1) {
                litmax = load(zmax, under(bits), bits, dim);
                zmin = load(zmin, over(bits), bits, dim);
                continue;
            }
            ZN_MATCH_ALL(1, 1, 0) {
                // Not possible, MIN <= MAX
                // assert(false);
                continue;
            }
            ZN_MATCH_ALL(1, 1, 1) { continue; }
        }
#undef ZN_MATCH_ALL

        return {litmax, bigmin};
    }

    /**
     * @brief Calculates the longest common binary prefix between two z longs
     *
     * @param a
     * @param b
     * @return (common prefix, number of bits in common)
     */
    ZPrefix max_common_prefix(uint64_t a, uint64_t b) {
        int32_t shift = TOTAL_BITS - DIMS;
        uint64_t head = a >> shift;
        while ((b >> shift) == head && shift > -1) {
            shift -= DIMS;
            head = a >> shift;
        }
        shift += DIMS;
        return {.prefix = a & (UINT64_MAX << shift), .precision = 64 - shift};
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
