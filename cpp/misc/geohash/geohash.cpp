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

#include "geohash.h"

#include <array>

namespace pl {

namespace {

constexpr inline std::string_view BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz";

// Limits from EPSG:900913 / EPSG:3785 / OSGEO:41001
constexpr inline double GEO_LAT_MIN = -85.05112878;
constexpr inline double GEO_LAT_MAX = 85.05112878;
constexpr inline double GEO_LNG_MIN = -180;
constexpr inline double GEO_LNG_MAX = 180;

constexpr inline uint8_t MAX_STEP = 32;

// Ref: https://graphics.stanford.edu/~seander/bithacks.html#InterleaveBMN
inline uint64_t interleave64(uint32_t xlo, uint32_t ylo) {
    constexpr static std::array<uint64_t, 5> B = {0x5555555555555555ULL, 0x3333333333333333ULL,
                                                  0x0F0F0F0F0F0F0F0FULL, 0x00FF00FF00FF00FFULL,
                                                  0x0000FFFF0000FFFFULL};
    constexpr static std::array<int, 5> S = {1, 2, 4, 8, 16};
    uint64_t x = xlo;
    uint64_t y = ylo;

    x = (x | (x << S[4])) & B[4];
    y = (y | (y << S[4])) & B[4];

    x = (x | (x << S[3])) & B[3];
    y = (y | (y << S[3])) & B[3];

    x = (x | (x << S[2])) & B[2];
    y = (y | (y << S[2])) & B[2];

    x = (x | (x << S[1])) & B[1];
    y = (y | (y << S[1])) & B[1];

    x = (x | (x << S[0])) & B[0];
    y = (y | (y << S[0])) & B[0];

    return x | (y << 1);
}

// Ref: http://stackoverflow.com/questions/4909263
inline uint64_t deinterleave64(uint64_t interleaved) {
    constexpr static std::array<uint64_t, 6> B = {0x5555555555555555ULL, 0x3333333333333333ULL,
                                                  0x0F0F0F0F0F0F0F0FULL, 0x00FF00FF00FF00FFULL,
                                                  0x0000FFFF0000FFFFULL, 0x00000000FFFFFFFFULL};
    constexpr static std::array<int, 6> S = {0, 1, 2, 4, 8, 16};

    uint64_t x = interleaved;
    uint64_t y = interleaved >> 1;

    x = (x | (x >> S[0])) & B[0];
    y = (y | (y >> S[0])) & B[0];

    x = (x | (x >> S[1])) & B[1];
    y = (y | (y >> S[1])) & B[1];

    x = (x | (x >> S[2])) & B[2];
    y = (y | (y >> S[2])) & B[2];

    x = (x | (x >> S[3])) & B[3];
    y = (y | (y >> S[3])) & B[3];

    x = (x | (x >> S[4])) & B[4];
    y = (y | (y >> S[4])) & B[4];

    x = (x | (x >> S[5])) & B[5];
    y = (y | (y >> S[5])) & B[5];

    return x | (y << 32);
}

} // namespace

bool GeoHash::encode(const Rectangle& range, double lng, double lat, uint8_t step, HashBits* hash) {
    // check basic arguments sanity
    if (range.is_zero() || hash == nullptr || step > MAX_STEP || step == 0) {
        return false;
    }

    if (lng > GEO_LNG_MAX || lng < GEO_LNG_MIN || lat > GEO_LAT_MAX || lat < GEO_LAT_MIN) {
        return false;
    }

    if (!range.contains(lng, lat)) {
        return false;
    }

    hash->bits = 0;
    hash->step = step;

    double lng_offset = (lng - range.min_lng()) / range.lng_scale();
    double lat_offset = (lat - range.min_lat()) / range.lat_scale();

    // convert to fixed point based on the step size
    lng_offset *= (1ULL << step);
    lat_offset *= (1ULL << step);

    hash->bits = interleave64(lat_offset, lng_offset);

    return true;
}

bool GeoHash::decode(const Rectangle& range, const HashBits& hash, Rectangle* area) {
    if (range.is_zero() || hash.is_zero() || area == nullptr) {
        return false;
    }

    uint8_t step = hash.step;
    uint64_t hash_sep = deinterleave64(hash.bits); // hash = [lat][lng]

    uint32_t ilato = hash_sep;       // get lat part of deinterleaved hash
    uint32_t ilono = hash_sep >> 32; // shift over to get long part of hash

    area->set_min_lat(range.min_lat() + (ilato * 1.0 / (1ULL << step)) * range.lat_scale());
    area->set_max_lat(range.min_lat() + ((ilato + 1) * 1.0 / (1ULL << step)) * range.lat_scale());
    area->set_min_lng(range.min_lng() + (ilono * 1.0 / (1ULL << step)) * range.lng_scale());
    area->set_max_lng(range.min_lng() + ((ilono + 1) * 1.0 / (1ULL << step)) * range.lng_scale());

    return true;
}

void GeoHash::move_x(HashBits* hash, int8_t d) {
    if (d == 0) {
        return;
    }

    uint64_t x = hash->bits & 0xaaaaaaaaaaaaaaaaULL;
    uint64_t y = hash->bits & 0x5555555555555555ULL;
    uint64_t z = 0x5555555555555555ULL >> (64 - hash->step * 2);

    if (d > 0) {
        x = x + (z + 1);
    } else {
        x = x | z;
        x = x - (z + 1);
    }

    x = x & (0xaaaaaaaaaaaaaaaaULL >> (64 - hash->step * 2));
    hash->bits = (x | y);
}

void GeoHash::move_y(HashBits* hash, int8_t d) {
    if (d == 0) {
        return;
    }

    uint64_t x = hash->bits & 0xaaaaaaaaaaaaaaaaULL;
    uint64_t y = hash->bits & 0x5555555555555555ULL;
    uint64_t z = 0xaaaaaaaaaaaaaaaaULL >> (64 - hash->step * 2);

    if (d > 0) {
        y = y + (z + 1);
    } else {
        y = y | z;
        y = y - (z + 1);
    }

    y = y & (0xaaaaaaaaaaaaaaaaULL >> (64 - hash->step * 2));
    hash->bits = (x | y);
}

void GeoHash::neighbors(const HashBits* hash, Neighbors* neighbors) {
    neighbors->e = *hash;
    neighbors->w = *hash;
    neighbors->n = *hash;
    neighbors->s = *hash;
    neighbors->se = *hash;
    neighbors->sw = *hash;
    neighbors->ne = *hash;
    neighbors->nw = *hash;

    // north
    move_x(&neighbors->n, 0);
    move_y(&neighbors->n, 1);

    // south
    move_x(&neighbors->s, 0);
    move_y(&neighbors->s, -1);

    // west
    move_x(&neighbors->w, -1);
    move_y(&neighbors->w, 0);

    // east
    move_x(&neighbors->e, 1);
    move_y(&neighbors->e, 0);

    // north-east
    move_x(&neighbors->ne, 1);
    move_y(&neighbors->ne, 1);

    // north-west
    move_x(&neighbors->nw, -1);
    move_y(&neighbors->nw, 1);

    // south-east
    move_x(&neighbors->se, 1);
    move_y(&neighbors->se, -1);

    // south-west
    move_x(&neighbors->sw, -1);
    move_y(&neighbors->sw, -1);
}

} // namespace pl
