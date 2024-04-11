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

#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace pl {

namespace {
constexpr inline std::string_view BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz";
constexpr inline double MIN_LAT = -90;
constexpr inline double MAX_LAT = 90;
constexpr inline double MIN_LNG = -180;
constexpr inline double MAX_LNG = 180;
constexpr inline std::size_t MAX_HASH_LEN = 12;
} // namespace

std::string_view GeoHash::encode(double lng, double lat, std::size_t precision, buffer_t& buffer) {
    assert(precision > 0 && precision <= MAX_HASH_LEN);
    double min_lat = MIN_LAT;
    double max_lat = MAX_LAT;
    double min_lng = MIN_LNG;
    double max_lng = MAX_LNG;

    bool even_bit = true;
    std::size_t ibx = 0;
    uint8_t bit = 0;
    std::size_t bit_len = 0;

    while (ibx < precision) {
        if (even_bit) {
            // 基数bit位是经度
            auto mid_lng = (min_lng + max_lng) / 2;
            if (lng > mid_lng) {
                bit = bit * 2 + 1;
                min_lng = mid_lng;
            } else {
                bit = bit * 2;
                max_lng = mid_lng;
            }
        } else {
            // 偶数bit位是纬度
            auto mid_lat = (min_lat + max_lat) / 2;
            if (lat > mid_lat) {
                bit = bit * 2 + 1;
                min_lat = mid_lat;
            } else {
                bit = bit * 2;
                max_lat = mid_lat;
            }
        }

        // 切换奇偶位
        even_bit = !even_bit;

        if (++bit_len == 5) {
            buffer[ibx++] = BASE32[bit];
            bit = 0;
            bit_len = 0;
        }
    }

    buffer[ibx] = '\0';

    return {buffer.data(), ibx};
}

GeoHash::Rectangle GeoHash::decode(std::string_view hash) {
    assert(hash.size() > 0 && hash.size() <= MAX_HASH_LEN);
    double min_lng = MIN_LNG;
    double max_lng = MAX_LNG;
    double min_lat = MIN_LAT;
    double max_lat = MAX_LAT;
    bool even_bit = true;

    for (char ch : hash) {
        // check if ch is a valid base32 char
        if (BASE32.find(ch) == std::string_view::npos) {
            throw std::logic_error("invalid geohash");
        }

        for (std::size_t j = 4; j >= 0; --j) {
            auto bit = (ch >> j) & 1;
            if (even_bit) {
                auto lng_mid = (min_lng + max_lng) / 2;
                if (bit == 0) {
                    min_lng = lng_mid;
                } else {
                    max_lng = lng_mid;
                }
            } else {
                auto lat_mid = (min_lat + max_lat) / 2;
                if (bit == 0) {
                    min_lat = lat_mid;
                } else {
                    max_lat = lat_mid;
                }
            }

            even_bit = !even_bit;
        }
    }

    return {
        {min_lng, min_lat},
        {max_lng, max_lat},
    };
}

std::string_view GeoHash::adjacent(std::string_view geohash,
                                   Direction direction,
                                   buffer_t& buffer) {
    assert(geohash.size() > 0 && geohash.size() < MAX_HASH_LEN);
    std::copy(geohash.begin(), geohash.end(), std::begin(buffer));
    return do_adjacent(geohash.size(), direction, buffer);
}

std::string_view GeoHash::do_adjacent(std::size_t size, Direction direction, buffer_t& buffer) {
    constexpr static std::string_view neighbour[4][2]{
        {"p0r21436x8zb9dcf5h7kjnmqesgutwvy", "bc01fg45238967deuvhjyznpkmstqrwx"},
        {"14365h7k9dcfesgujnmqp0r2twvyx8zb", "238967debc01fg45kmstqrwxuvhjyznp"},
        {"bc01fg45238967deuvhjyznpkmstqrwx", "p0r21436x8zb9dcf5h7kjnmqesgutwvy"},
        {"238967debc01fg45kmstqrwxuvhjyznp", "14365h7k9dcfesgujnmqp0r2twvyx8zb"}};

    constexpr static std::string_view border[4][2]{
        {"prxz", "bcfguvyz"}, {"028b", "0145hjnp"}, {"bcfguvyz", "prxz"}, {"0145hjnp", "028b"}};

    const auto lastchar = buffer[size - 1];
    const auto type = size % 2;
    const auto idx = static_cast<uint8_t>(direction);

    if (border[idx][type].find(lastchar) != std::string_view::npos && size > 1) {
        do_adjacent(size - 1, direction, buffer);
    }

    // append letter for direction to parent
    buffer[size - 1] = BASE32[neighbour[idx][type].find(lastchar)];
    return {buffer.data(), size};
}

} // namespace pl
