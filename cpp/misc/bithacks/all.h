//=====================================================================
//
// all.h -
//
// Created by liubang on 2023/10/25 19:34
// Last Modified: 2023/10/25 19:34
//
//=====================================================================

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

} // namespace pl
