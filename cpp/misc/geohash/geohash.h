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

#include <cstdint>
#include <ostream>

namespace pl {

class GeoHash {
public:
    // Limits from EPSG:900913 / EPSG:3785 / OSGEO:41001
    static constexpr double GEO_LAT_MIN = -85.05112878;
    static constexpr double GEO_LAT_MAX = 85.05112878;
    static constexpr double GEO_LNG_MIN = -180;
    static constexpr double GEO_LNG_MAX = 180;
    static constexpr uint8_t GEO_MAX_STEP = 32; /* 32 * 2 = 64 bits */

public:
    struct Point {
        double lng; // 经度
        double lat; // 纬度

        [[nodiscard]] constexpr bool is_zero() const { return lng == 0 && lat == 0; }

        friend std::ostream& operator<<(std::ostream& os, const Point& point) {
            os << "{lng: " << point.lng << ", lat: " << point.lat << "}";
            return os;
        }
    };

    struct Area {
        Point sw; // 西南
        Point ne; // 东北

        void set_min_lat(double lat) { sw.lat = lat; }

        void set_max_lat(double lat) { ne.lat = lat; }

        void set_min_lng(double lng) { sw.lng = lng; }

        void set_max_lng(double lng) { ne.lng = lng; }

        [[nodiscard]] constexpr Point center() const {
            return {
                (sw.lng + ne.lng) / 2,
                (sw.lat + ne.lat) / 2,
            };
        };

        [[nodiscard]] constexpr bool is_zero() const { return sw.is_zero() || ne.is_zero(); }

        [[nodiscard]] double min_lat() const { return sw.lat; }

        [[nodiscard]] double max_lat() const { return ne.lat; }

        [[nodiscard]] double min_lng() const { return sw.lng; }

        [[nodiscard]] double max_lng() const { return ne.lng; }

        [[nodiscard]] double lat_scale() const { return ne.lat - sw.lat; }

        [[nodiscard]] double lng_scale() const { return ne.lng - sw.lng; }

        [[nodiscard]] bool contains(double lng, double lat) const {
            return lng >= min_lng() && lng <= max_lng() && lat >= min_lat() && lat <= max_lat();
        }

        friend std::ostream& operator<<(std::ostream& os, const Area& rectangle) {
            os << "{sw: " << rectangle.sw << ", ne: " << rectangle.ne << "}";
            return os;
        }
    };

    struct HashBits {
        uint64_t bits;
        uint8_t step;

        [[nodiscard]] constexpr bool is_zero() const { return bits == 0 && step == 0; }
    };

    struct Neighbors {
        HashBits n;
        HashBits e;
        HashBits w;
        HashBits s;
        HashBits ne;
        HashBits nw;
        HashBits se;
        HashBits sw;
    };

    struct GeoShape {
        enum {
            CIRCULAR_TYPE,
            RECTANGLE_TYPE,
        } type;
        Point center;      // search center point
        double conversion; // km: 1000
        Area bounds;
        union {
            double radius;
            struct {
                double height;
                double width;
            } r;
        } t;
    };

    struct GeoHashRadius {
        HashBits hash;
        Area area;
        Neighbors neighbors;
    };

public:
    static bool encode(const Area& range, double lng, double lat, uint8_t step, HashBits* hash);

    static bool encode_wgs84(double lng, double lat, uint8_t step, HashBits* hash);

    static bool decode(const Area& range, const HashBits& hash, Area* area);

    static bool decode_wgs84(const HashBits& hash, Area* area);

    static bool decode_area_to_point(const Area& area, Point* point);

    static bool decode_to_point_wgs84(const HashBits& hash, Point* point);

    static void neighbors(const HashBits* hash, Neighbors* neighbors);

private:
    static void move_x(HashBits* hash, int8_t d);

    static void move_y(HashBits* hash, int8_t d);
};

} // namespace pl
