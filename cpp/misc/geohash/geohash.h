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

#include <ostream>
#include <string_view>

namespace pl {

class GeoHash {
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

    struct Rectangle {
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

        friend std::ostream& operator<<(std::ostream& os, const Rectangle& rectangle) {
            os << "{sw: " << rectangle.sw << ", ne: " << rectangle.ne << "}";
            return os;
        }
    };

    struct HashBits {
        uint64_t bits;
        uint8_t step;

        [[nodiscard]] constexpr bool is_zero() const { return bits == 0 && step == 0; }
    };

public:
    bool encode(const Rectangle& range, double lng, double lat, uint8_t step, HashBits* hash);

    bool decode(const Rectangle& range, const HashBits& hash, Rectangle* area);

private:
    enum class Direction : uint8_t {
        N = 0,
        S = 1,
        E = 2,
        W = 3,
    };
};

} // namespace pl
