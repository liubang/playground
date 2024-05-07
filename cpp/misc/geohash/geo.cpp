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

#include "geo.h"

namespace pl {

double Geo::geo_distance(const GeoHash::Point& p1, const GeoHash::Point& p2) {
    double lng1r = deg_rad(p1.lng);
    double lng2r = deg_rad(p2.lng);
    double v = std::sin((lng2r - lng1r) / 2);
    if (v == 0.0) {
        return geo_lat_distance(p1.lat, p2.lat);
    }
    double lat1r = deg_rad(p1.lat);
    double lat2r = deg_rad(p2.lat);
    double u = std::sin((lat2r - lat1r) / 2);
    double a = u * u + std::cos(lat1r) * std::cos(lat2r) * v * v;
    return 2.0 * EARTH_RADIUS_IN_METERS * std::asin(std::sqrt(a));
}

bool Geo::geo_get_distance_if_in_radius(const GeoHash::Point& p1,
                                        const GeoHash::Point& p2,
                                        double radius,
                                        double* distance) {
    *distance = geo_distance(p1, p2);
    return *distance <= radius;
}

bool Geo::geo_get_distance_if_in_rectangle(double width_m,
                                           double height_m,
                                           const GeoHash::Point& p1,
                                           const GeoHash::Point& p2,
                                           double* distance) {
    double lat_distance = geo_lat_distance(p2.lat, p1.lat);
    if (lat_distance > height_m / 2) {
        return false;
    }
    double lng_distance = geo_distance(p2, p1);
    if (lng_distance > width_m / 2) {
        return false;
    }
    *distance = geo_distance(p1, p2);
    return true;
}

uint8_t Geo::estimate_steps_by_radius(double range_meters, double lat) {
    if (range_meters == 0) {
        return 26;
    }
    int step = 1;
    while (range_meters < MERCATOR_MAX) {
        range_meters *= 2;
        step++;
    }
    // make sure range is included in most of the base areas
    step -= 2;

    if (std::abs(lat) > 66) {
        step--;
        if (std::abs(lat) > 80) {
            step--;
        }
    }
    if (step < 1) {
        step = 1;
    }
    if (step > 26) {
        step = 26;
    }
    return step;
}

bool Geo::geohash_bouding_box(const GeoHash::GeoShape& shape, GeoHash::Area* bounds) {
    if (bounds == nullptr) {
        return false;
    }
    double lat = shape.center.lat;
    double lng = shape.center.lng;
    double height =
        shape.conversion *
        (shape.type == GeoHash::GeoShape::CIRCULAR_TYPE ? shape.t.radius : shape.t.r.height / 2);
    double width =
        shape.conversion *
        (shape.type == GeoHash::GeoShape::CIRCULAR_TYPE ? shape.t.radius : shape.t.r.width / 2);

    double lat_delta = rad_deg(height / EARTH_RADIUS_IN_METERS);
    double lng_delta_top =
        rad_deg(width / EARTH_RADIUS_IN_METERS / std::cos(deg_rad(lat + lat_delta)));
    double lng_delta_bottom =
        rad_deg(width / EARTH_RADIUS_IN_METERS / std::cos(deg_rad(lat - lat_delta)));
    bool southern_hemishpere = lat < 0;
    bounds->set_min_lng(southern_hemishpere ? lng - lng_delta_bottom : lng - lng_delta_top);
    bounds->set_max_lng(southern_hemishpere ? lng + lng_delta_bottom : lng + lng_delta_top);
    bounds->set_min_lat(lat - lat_delta);
    bounds->set_max_lat(lat + lat_delta);
    return true;
}

void Geo::geohash_cal_area_by_shape(const GeoHash::GeoShape& shape,
                                    GeoHash::GeoHashRadius* radius) {}

uint64_t Geo::geohash_align52bits(const GeoHash::HashBits& hash) {
    uint64_t bits = hash.bits;
    bits <<= (52 - hash.step * 2);
    return bits;
}

} // namespace pl
