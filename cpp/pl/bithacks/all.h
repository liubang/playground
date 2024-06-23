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

#include <climits>

namespace pl {

/**
 * @brief Compute the sign of an integer
 *
 * @param int v
 * @return if v < 0 then -1, else 0
 */
inline int sign_of_integer(int v) {
    return -(int)((unsigned int)((int)v) >> (sizeof(int) * CHAR_BIT - 1));
}

/**
 * @brief detect if two integers have opposite signs
 *
 * @param int x
 * @param int y
 * @return true iff x and y have opposite signs
 */
inline bool opposite_signs(int x, int y) { return (x ^ y) < 0; }

/**
 * @brief compute the integer absolute value without branching
 *
 * @param int v
 * @return int
 */
inline unsigned int abs(int v) {
    const int mask = v >> (sizeof(int) * CHAR_BIT - 1);
    return (v ^ mask) - mask;
}

/**
 * @brief compute minimum of two integers without branching
 *
 * @param int x
 * @param int y
 * @return int
 */
inline int min(int x, int y) { return y ^ ((x ^ y) & -static_cast<int>(x < y)); }

/**
 * @brief compute maximum of two integers without branching
 *
 * @param int x
 * @param int y
 * @return int
 */
inline int max(int x, int y) { return x ^ ((x ^ y) & -static_cast<int>(x < y)); }

/**
 * @brief determining if an integer is a power of 2
 *
 * @param int v
 * @return bool
 */
inline bool power_of_2(int v) { return (v != 0) && ((v & (v - 1)) == 0); }

/**
 * @brief sign extending from a constant bit-width
 *
 */
template <typename T, unsigned B> inline T signextend(const T x) {
    struct {
        T x : B;
    } __s;
    return __s.x = x;
}

/**
 * @brief conditionally set or clear bits without branching
 *
 * @param bool f conditional flag
 * @param unsigned int m the bit mask
 * @param unsigned int w the word to modify: if (f) w |= m; else w &= ~m;
 */
inline unsigned int clear_bits_with_condition(bool f, unsigned int m, unsigned int w) {
    w ^= (-static_cast<int>(f) ^ w) & m;
    return w;
}

/**
 * @brief counting bits set by lookup table
 *
 * @param unsigned int v count the number of bits set in 32-bit value v
 * @return unsigned int c is the total bits set in v
 */
inline unsigned int count_bitset_by_table(unsigned int v) {
    static const unsigned char bitsset_table256[256] = {
#define B2(n) n, n + 1, n + 1, n + 2
#define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
        B6(0), B6(1), B6(1), B6(2)};

    unsigned int c;
    auto* p = (unsigned char*)&v;
    c = bitsset_table256[p[0]] + bitsset_table256[p[1]] + bitsset_table256[p[2]] +
        bitsset_table256[p[3]];

    return c;
}

inline unsigned int count_bitset(unsigned int v) {
    unsigned int c;
    for (c = 0; v != 0u; c++) {
        v &= v - 1; // clear the least significant bit set
    }
    return c;
}

} // namespace pl
