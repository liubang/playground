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

#include <array>
#include <string_view>

namespace pl {

using buffer_t = std::array<char, 32>;

class GeoHash {
public:
    struct Point {
        double lng; // 经度
        double lat; // 纬度
    };

    struct Rectangle {
        Point sw; // 西南
        Point ne; // 东北

        [[nodiscard]] constexpr Point center() const {
            return {
                (sw.lng + ne.lng) / 2,
                (sw.lat + ne.lat) / 2,
            };
        };
    };

    /**
     * @param lng Longitude in degrees
     * @param lat Latitude in degrees
     * @param precision Number of characters in resulting geohash
     * @param buffer Geohash
     */
    static std::string_view encode(double lng, double lat, std::size_t precision, buffer_t& buffer);

    /**
     * @param hash Geohash
     * @return SW/NE latitude/longitude bounds of specified geohash
     */
    static Rectangle decode(std::string_view hash);
};

} // namespace pl
