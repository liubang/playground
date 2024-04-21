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

#include <cmath>

namespace pl {

class Geo {
public:
    static double geo_distance(const GeoHash::Point& p1, const GeoHash::Point& p2);

    static std::string geohash_string(const GeoHash::HashBits& hash);

    static uint64_t geohash_align52bits(const GeoHash::HashBits& hash);

private:
    static constexpr double D_R = (M_PI / 180.0);
    // The usual PI/180 constant
    static constexpr double DEG_TO_RAD = 0.017453292519943295769236907684886;
    // Earth's quatratic mean radius for WGS-84
    static constexpr double EARTH_RADIUS_IN_METERS = 6372797.560856;

private:
    static inline double deg_rad(double ang) { return ang * D_R; }
    static inline double rad_deg(double ang) { return ang / D_R; }

    static inline double geo_lat_distance(double lat1d, double lat2d) {
        return EARTH_RADIUS_IN_METERS * std::fabs(deg_rad(lat2d) - deg_rad(lat1d));
    }
};

} // namespace pl
