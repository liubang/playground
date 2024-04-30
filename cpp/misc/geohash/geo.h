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
#include <set>

namespace pl {

class GeoHashBitsComparator {
    bool comparator(const GeoHash::HashBits& a, const GeoHash::HashBits& b) {
        if (a.step < b.step) {
            return true;
        }
        if (a.step > b.step) {
            return false;
        }
        return a.bits < b.bits;
    }
};

using GeoHashBitsSet = std::set<GeoHash::HashBits, GeoHashBitsComparator>;

class Geo {
public:
    static double geo_distance(const GeoHash::Point& p1, const GeoHash::Point& p2);

    static bool geo_get_distance_if_in_radius(const GeoHash::Point& p1,
                                              const GeoHash::Point& p2,
                                              double radius,
                                              double* distance);
    /**
     * @brief Judge whether a point is in the axis-aligned rectangle, when the distance between a
     * searched point and the center point is less than or equal to height/2 or width /2 in height
     * and width, the point is the rectangle.
     *
     * @param width_m the rectangle width
     * @param height_m the rectangle height
     * @param p1 the center of the box
     * @param p2 the point to be searched
     * @param distance
     * @return
     */
    static bool geo_get_distance_if_in_rectangle(double width_m,
                                                 double height_m,
                                                 const GeoHash::Point& p1,
                                                 const GeoHash::Point& p2,
                                                 double* distance);

    /**
     * @brief return the bounding box of the search area by shape
     *
     * @param shape 
     * @param bounds 
     * @return 
     */
    static bool geohash_bouding_box(const GeoHash::GeoShape& shape, GeoHash::Area* bounds);

    static uint64_t geohash_align52bits(const GeoHash::HashBits& hash);

private:
    static constexpr double D_R = (M_PI / 180.0);
    static constexpr double R_MAJOR = 6378137.0;
    static constexpr double R_MINOR = 6356752.3142;
    static constexpr double RATIO = (R_MINOR / R_MAJOR);
    // The usual PI/180 constant
    static constexpr double DEG_TO_RAD = 0.017453292519943295769236907684886;
    // Earth's quatratic mean radius for WGS-84
    static constexpr double EARTH_RADIUS_IN_METERS = 6372797.560856;
    static constexpr double MERCATOR_MAX = 20037726.37;
    static constexpr double MERCATOR_MIN = -20037726.37;

private:
    static inline double deg_rad(double ang) { return ang * D_R; }
    static inline double rad_deg(double ang) { return ang / D_R; }

    static inline double geo_lat_distance(double lat1d, double lat2d) {
        return EARTH_RADIUS_IN_METERS * std::fabs(deg_rad(lat2d) - deg_rad(lat1d));
    }

    /**
     * @brief this function is used in order to estimate the step(bits precision) of the 9 search
     * area boxes during radius queries
     *
     * @param range_meters
     * @param lat
     * @return
     */
    static uint8_t estimate_steps_by_radius(double range_meters, double lat);
};

} // namespace pl
