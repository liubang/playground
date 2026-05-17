// Copyright (c) 2023 The Authors. All rights reserved.
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
// Created: 2023/06/04 20:10

#include "geo.h"
#include "geohash.h"
#include <cmath>
#include <gtest/gtest.h>

// ============================================================================
// GeoHash encode/decode tests
// ============================================================================

TEST(GeoHashTest, EncodeWGS84Basic) {
    pl::GeoHash::HashBits hash{};
    // Beijing: lng=116.31, lat=40.04
    bool ok = pl::GeoHash::encode_wgs84(116.31, 40.04, 26, &hash);
    ASSERT_TRUE(ok);
    EXPECT_EQ(26, hash.step);
    EXPECT_NE(0ULL, hash.bits);
}

TEST(GeoHashTest, EncodeDecodeRoundTrip) {
    // Encode a known point, decode it, verify the point falls within the area
    pl::GeoHash::HashBits hash{};
    double lng = 116.397128;
    double lat = 39.916527;
    bool ok = pl::GeoHash::encode_wgs84(lng, lat, 26, &hash);
    ASSERT_TRUE(ok);

    pl::GeoHash::Area area{};
    ok = pl::GeoHash::decode_wgs84(hash, &area);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(area.contains(lng, lat));
    // The area should be very small at step=26
    EXPECT_LT(area.lng_scale(), 0.001);
    EXPECT_LT(area.lat_scale(), 0.001);
}

TEST(GeoHashTest, EncodeDecodeToPointRoundTrip) {
    double lng = -73.985428; // New York
    double lat = 40.748817;
    pl::GeoHash::HashBits hash{};
    bool ok = pl::GeoHash::encode_wgs84(lng, lat, 26, &hash);
    ASSERT_TRUE(ok);

    pl::GeoHash::Point point{};
    ok = pl::GeoHash::decode_to_point_wgs84(hash, &point);
    ASSERT_TRUE(ok);

    // The decoded point should be very close to the original
    EXPECT_NEAR(lng, point.lng, 0.001);
    EXPECT_NEAR(lat, point.lat, 0.001);
}

TEST(GeoHashTest, EncodeDifferentSteps) {
    double lng = 116.397128;
    double lat = 39.916527;

    // Higher steps give more precision (narrower area)
    pl::GeoHash::HashBits hash_low{}, hash_high{};
    pl::GeoHash::encode_wgs84(lng, lat, 5, &hash_low);
    pl::GeoHash::encode_wgs84(lng, lat, 26, &hash_high);

    pl::GeoHash::Area area_low{}, area_high{};
    pl::GeoHash::decode_wgs84(hash_low, &area_low);
    pl::GeoHash::decode_wgs84(hash_high, &area_high);

    EXPECT_GT(area_low.lng_scale(), area_high.lng_scale());
    EXPECT_GT(area_low.lat_scale(), area_high.lat_scale());
}

TEST(GeoHashTest, EncodeInvalidNullHash) {
    bool ok = pl::GeoHash::encode_wgs84(116.0, 40.0, 26, nullptr);
    EXPECT_FALSE(ok);
}

TEST(GeoHashTest, EncodeInvalidStep) {
    pl::GeoHash::HashBits hash{};
    // step=0 is invalid
    bool ok = pl::GeoHash::encode_wgs84(116.0, 40.0, 0, &hash);
    EXPECT_FALSE(ok);

    // step > 32 is invalid
    ok = pl::GeoHash::encode_wgs84(116.0, 40.0, 33, &hash);
    EXPECT_FALSE(ok);
}

TEST(GeoHashTest, EncodeOutOfRange) {
    pl::GeoHash::HashBits hash{};
    // Longitude out of range
    EXPECT_FALSE(pl::GeoHash::encode_wgs84(181.0, 40.0, 26, &hash));
    EXPECT_FALSE(pl::GeoHash::encode_wgs84(-181.0, 40.0, 26, &hash));

    // Latitude out of range
    EXPECT_FALSE(pl::GeoHash::encode_wgs84(116.0, 86.0, 26, &hash));
    EXPECT_FALSE(pl::GeoHash::encode_wgs84(116.0, -86.0, 26, &hash));
}

TEST(GeoHashTest, EncodeCustomRange) {
    pl::GeoHash::Area range{
        pl::GeoHash::Point{100.0, 30.0},
        pl::GeoHash::Point{120.0, 50.0},
    };
    pl::GeoHash::HashBits hash{};

    // Point within custom range
    bool ok = pl::GeoHash::encode(range, 110.0, 40.0, 20, &hash);
    ASSERT_TRUE(ok);

    // Point outside custom range
    ok = pl::GeoHash::encode(range, 90.0, 40.0, 20, &hash);
    EXPECT_FALSE(ok);
}

TEST(GeoHashTest, DecodeInvalidArgs) {
    pl::GeoHash::Area range{
        pl::GeoHash::Point{-180, -85.05112878},
        pl::GeoHash::Point{180, 85.05112878},
    };

    // Null area pointer
    pl::GeoHash::HashBits hash{10, 5};
    bool ok = pl::GeoHash::decode(range, hash, nullptr);
    EXPECT_FALSE(ok);

    // Zero hash
    pl::GeoHash::HashBits zero_hash{0, 0};
    pl::GeoHash::Area area{};
    ok = pl::GeoHash::decode(range, zero_hash, &area);
    EXPECT_FALSE(ok);
}

TEST(GeoHashTest, DecodeAreaToPointNullptr) {
    pl::GeoHash::Area area{
        pl::GeoHash::Point{116.0, 39.0},
        pl::GeoHash::Point{117.0, 40.0},
    };
    bool ok = pl::GeoHash::decode_area_to_point(area, nullptr);
    EXPECT_FALSE(ok);
}

TEST(GeoHashTest, DecodeAreaToPointClampsBounds) {
    // Create an area that extends beyond GEO limits
    pl::GeoHash::Area area{
        pl::GeoHash::Point{-200.0, -90.0},
        pl::GeoHash::Point{200.0, 90.0},
    };
    pl::GeoHash::Point point{};
    bool ok = pl::GeoHash::decode_area_to_point(area, &point);
    ASSERT_TRUE(ok);
    // Center would be (0,0) but check it's clamped within bounds
    EXPECT_GE(point.lng, pl::GeoHash::GEO_LNG_MIN);
    EXPECT_LE(point.lng, pl::GeoHash::GEO_LNG_MAX);
    EXPECT_GE(point.lat, pl::GeoHash::GEO_LAT_MIN);
    EXPECT_LE(point.lat, pl::GeoHash::GEO_LAT_MAX);
}

TEST(GeoHashTest, DecodeToPointWGS84NullPtr) {
    pl::GeoHash::HashBits hash{100, 10};
    bool ok = pl::GeoHash::decode_to_point_wgs84(hash, nullptr);
    EXPECT_FALSE(ok);
}

// ============================================================================
// GeoHash neighbors tests
// ============================================================================

TEST(GeoHashTest, NeighborsSymmetry) {
    pl::GeoHash::HashBits hash{};
    pl::GeoHash::encode_wgs84(116.397128, 39.916527, 20, &hash);

    pl::GeoHash::Neighbors nb{};
    pl::GeoHash::neighbors(&hash, &nb);

    // All neighbors should have the same step
    EXPECT_EQ(hash.step, nb.n.step);
    EXPECT_EQ(hash.step, nb.s.step);
    EXPECT_EQ(hash.step, nb.e.step);
    EXPECT_EQ(hash.step, nb.w.step);
    EXPECT_EQ(hash.step, nb.ne.step);
    EXPECT_EQ(hash.step, nb.nw.step);
    EXPECT_EQ(hash.step, nb.se.step);
    EXPECT_EQ(hash.step, nb.sw.step);

    // All neighbors should be different from center
    EXPECT_NE(hash.bits, nb.n.bits);
    EXPECT_NE(hash.bits, nb.s.bits);
    EXPECT_NE(hash.bits, nb.e.bits);
    EXPECT_NE(hash.bits, nb.w.bits);
    EXPECT_NE(hash.bits, nb.ne.bits);
    EXPECT_NE(hash.bits, nb.nw.bits);
    EXPECT_NE(hash.bits, nb.se.bits);
    EXPECT_NE(hash.bits, nb.sw.bits);
}

TEST(GeoHashTest, NeighborsAdjacency) {
    // Verify that decoded neighbor areas are actually adjacent to center
    // Use a moderate step and central location to avoid wraparound
    pl::GeoHash::HashBits hash{};
    pl::GeoHash::encode_wgs84(0.0, 0.0, 10, &hash);

    pl::GeoHash::Neighbors nb{};
    pl::GeoHash::neighbors(&hash, &nb);

    pl::GeoHash::Area center_area{};
    pl::GeoHash::decode_wgs84(hash, &center_area);

    // East neighbor's center should have higher longitude
    pl::GeoHash::Area east_area{};
    pl::GeoHash::decode_wgs84(nb.e, &east_area);
    EXPECT_GT(east_area.center().lng, center_area.center().lng);

    // West neighbor's center should have lower longitude
    pl::GeoHash::Area west_area{};
    pl::GeoHash::decode_wgs84(nb.w, &west_area);
    EXPECT_LT(west_area.center().lng, center_area.center().lng);

    // North neighbor's center should have higher latitude
    pl::GeoHash::Area north_area{};
    pl::GeoHash::decode_wgs84(nb.n, &north_area);
    EXPECT_GT(north_area.center().lat, center_area.center().lat);

    // South neighbor's center should have lower latitude
    pl::GeoHash::Area south_area{};
    pl::GeoHash::decode_wgs84(nb.s, &south_area);
    EXPECT_LT(south_area.center().lat, center_area.center().lat);
}

TEST(GeoHashTest, NeighborsDontOverlap) {
    pl::GeoHash::HashBits hash{};
    pl::GeoHash::encode_wgs84(0.0, 0.0, 10, &hash);

    pl::GeoHash::Neighbors nb{};
    pl::GeoHash::neighbors(&hash, &nb);

    // All 8 neighbors should be distinct from each other
    std::set<uint64_t> unique_bits;
    unique_bits.insert(hash.bits);
    unique_bits.insert(nb.n.bits);
    unique_bits.insert(nb.s.bits);
    unique_bits.insert(nb.e.bits);
    unique_bits.insert(nb.w.bits);
    unique_bits.insert(nb.ne.bits);
    unique_bits.insert(nb.nw.bits);
    unique_bits.insert(nb.se.bits);
    unique_bits.insert(nb.sw.bits);
    EXPECT_EQ(9, unique_bits.size());
}

// ============================================================================
// GeoHash align52bits tests
// ============================================================================

TEST(GeoHashTest, Align52Bits) {
    pl::GeoHash::HashBits hash{};
    pl::GeoHash::encode_wgs84(116.397128, 39.916527, 26, &hash);

    uint64_t aligned = pl::Geo::geohash_align52bits(hash);
    // 26 steps = 52 bits used, so align should be no-op shift
    EXPECT_EQ(hash.bits, aligned);
}

TEST(GeoHashTest, Align52BitsSmallStep) {
    pl::GeoHash::HashBits hash{0b1010, 2}; // 2 steps = 4 bits
    uint64_t aligned = pl::Geo::geohash_align52bits(hash);
    // Should shift left by 52 - 2*2 = 48 bits
    EXPECT_EQ(hash.bits << 48, aligned);
}

// ============================================================================
// Geo distance tests
// ============================================================================

TEST(GeoTest, GeoDistanceSamePoint) {
    pl::GeoHash::Point p{116.397128, 39.916527};
    double d = pl::Geo::geo_distance(p, p);
    EXPECT_DOUBLE_EQ(0.0, d);
}

TEST(GeoTest, GeoDistanceKnownPoints) {
    // Beijing to Shanghai, approximately 1068 km
    pl::GeoHash::Point beijing{116.397128, 39.916527};
    pl::GeoHash::Point shanghai{121.473701, 31.230416};
    double d = pl::Geo::geo_distance(beijing, shanghai);
    // Allow 5% tolerance for Haversine approximation
    EXPECT_NEAR(1068000.0, d, 60000.0);
}

TEST(GeoTest, GeoDistanceSameLongitude) {
    // Two points on same longitude, separated by 1 degree of latitude
    pl::GeoHash::Point p1{116.0, 40.0};
    pl::GeoHash::Point p2{116.0, 41.0};
    double d = pl::Geo::geo_distance(p1, p2);
    // 1 degree of latitude ≈ 111 km
    EXPECT_NEAR(111000.0, d, 2000.0);
}

TEST(GeoTest, GeoDistanceAntipodal) {
    // Points on opposite sides of Earth
    pl::GeoHash::Point p1{0.0, 0.0};
    pl::GeoHash::Point p2{180.0, 0.0};
    double d = pl::Geo::geo_distance(p1, p2);
    // Half circumference ≈ 20015 km
    EXPECT_NEAR(20015000.0, d, 100000.0);
}

// ============================================================================
// Geo distance-in-radius tests
// ============================================================================

TEST(GeoTest, GetDistanceIfInRadiusInside) {
    pl::GeoHash::Point center{116.397128, 39.916527};
    // A point ~500m away
    pl::GeoHash::Point nearby{116.402, 39.916527};
    double distance = 0;
    bool in_radius = pl::Geo::geo_get_distance_if_in_radius(center, nearby, 1000.0, &distance);
    EXPECT_TRUE(in_radius);
    EXPECT_GT(distance, 0.0);
    EXPECT_LE(distance, 1000.0);
}

TEST(GeoTest, GetDistanceIfInRadiusOutside) {
    pl::GeoHash::Point center{116.397128, 39.916527};
    // Shanghai is far away
    pl::GeoHash::Point shanghai{121.473701, 31.230416};
    double distance = 0;
    bool in_radius = pl::Geo::geo_get_distance_if_in_radius(center, shanghai, 1000.0, &distance);
    EXPECT_FALSE(in_radius);
    EXPECT_GT(distance, 1000.0);
}

// ============================================================================
// Geo distance-in-rectangle tests
// ============================================================================

TEST(GeoTest, GetDistanceIfInRectangleInside) {
    pl::GeoHash::Point center{116.397128, 39.916527};
    // A point very close
    pl::GeoHash::Point nearby{116.398, 39.917};
    double distance = 0;
    // 1km x 1km rectangle
    bool in_rect =
        pl::Geo::geo_get_distance_if_in_rectangle(1000.0, 1000.0, center, nearby, &distance);
    EXPECT_TRUE(in_rect);
    EXPECT_GT(distance, 0.0);
}

TEST(GeoTest, GetDistanceIfInRectangleOutsideLat) {
    pl::GeoHash::Point center{116.397128, 39.916527};
    // ~2 degrees north ≈ 222 km in latitude
    pl::GeoHash::Point far_north{116.397128, 41.916527};
    double distance = 0;
    // 1km x 1km rectangle
    bool in_rect =
        pl::Geo::geo_get_distance_if_in_rectangle(1000.0, 1000.0, center, far_north, &distance);
    EXPECT_FALSE(in_rect);
}

TEST(GeoTest, GetDistanceIfInRectangleOutsideLng) {
    pl::GeoHash::Point center{116.397128, 39.916527};
    // ~2 degrees east, far in longitude
    pl::GeoHash::Point far_east{118.397128, 39.916527};
    double distance = 0;
    bool in_rect =
        pl::Geo::geo_get_distance_if_in_rectangle(1000.0, 1000.0, center, far_east, &distance);
    EXPECT_FALSE(in_rect);
}

// ============================================================================
// Geo bounding box tests
// ============================================================================

TEST(GeoTest, BoundingBoxCircular) {
    pl::GeoHash::GeoShape shape{};
    shape.type = pl::GeoHash::GeoShape::CIRCULAR_TYPE;
    shape.center = {116.397128, 39.916527};
    shape.conversion = 1000.0; // meters per unit
    shape.t.radius = 1.0;      // 1 km

    pl::GeoHash::Area bounds{};
    bool ok = pl::Geo::geohash_bouding_box(shape, &bounds);
    ASSERT_TRUE(ok);

    // Center should be within bounds
    EXPECT_TRUE(bounds.contains(shape.center.lng, shape.center.lat));
    // Bounds should extend beyond center
    EXPECT_LT(bounds.min_lng(), shape.center.lng);
    EXPECT_GT(bounds.max_lng(), shape.center.lng);
    EXPECT_LT(bounds.min_lat(), shape.center.lat);
    EXPECT_GT(bounds.max_lat(), shape.center.lat);
}

TEST(GeoTest, BoundingBoxRectangle) {
    pl::GeoHash::GeoShape shape{};
    shape.type = pl::GeoHash::GeoShape::RECTANGLE_TYPE;
    shape.center = {116.397128, 39.916527};
    shape.conversion = 1000.0;
    shape.t.r.width = 2.0;  // 2 km wide
    shape.t.r.height = 1.0; // 1 km tall

    pl::GeoHash::Area bounds{};
    bool ok = pl::Geo::geohash_bouding_box(shape, &bounds);
    ASSERT_TRUE(ok);

    EXPECT_TRUE(bounds.contains(shape.center.lng, shape.center.lat));
    // Width > height so lng extent should be larger than lat extent
    double lng_extent = bounds.max_lng() - bounds.min_lng();
    double lat_extent = bounds.max_lat() - bounds.min_lat();
    EXPECT_GT(lng_extent, lat_extent);
}

TEST(GeoTest, BoundingBoxNullptr) {
    pl::GeoHash::GeoShape shape{};
    shape.type = pl::GeoHash::GeoShape::CIRCULAR_TYPE;
    shape.center = {116.0, 40.0};
    shape.conversion = 1000.0;
    shape.t.radius = 1.0;

    bool ok = pl::Geo::geohash_bouding_box(shape, nullptr);
    EXPECT_FALSE(ok);
}

// ============================================================================
// Geo cal_area_by_shape tests
// ============================================================================

TEST(GeoTest, CalAreaByShapeCircular) {
    pl::GeoHash::GeoShape shape{};
    shape.type = pl::GeoHash::GeoShape::CIRCULAR_TYPE;
    shape.center = {116.397128, 39.916527};
    shape.conversion = 1000.0;
    shape.t.radius = 5.0; // 5 km

    pl::GeoHash::GeoHashRadius result{};
    pl::Geo::geohash_cal_area_by_shape(shape, &result);

    // Center hash should be valid
    EXPECT_NE(0ULL, result.hash.bits);
    EXPECT_GT(result.hash.step, 0);
    EXPECT_LE(result.hash.step, 26);

    // Area should contain the center point
    EXPECT_TRUE(result.area.contains(shape.center.lng, shape.center.lat));

    // All neighbors should be valid and distinct
    std::set<uint64_t> unique_bits;
    unique_bits.insert(result.hash.bits);
    unique_bits.insert(result.neighbors.n.bits);
    unique_bits.insert(result.neighbors.s.bits);
    unique_bits.insert(result.neighbors.e.bits);
    unique_bits.insert(result.neighbors.w.bits);
    unique_bits.insert(result.neighbors.ne.bits);
    unique_bits.insert(result.neighbors.nw.bits);
    unique_bits.insert(result.neighbors.se.bits);
    unique_bits.insert(result.neighbors.sw.bits);
    EXPECT_EQ(9, unique_bits.size());
}

TEST(GeoTest, CalAreaByShapeRectangle) {
    pl::GeoHash::GeoShape shape{};
    shape.type = pl::GeoHash::GeoShape::RECTANGLE_TYPE;
    shape.center = {-73.985428, 40.748817}; // New York
    shape.conversion = 1000.0;
    shape.t.r.width = 10.0;
    shape.t.r.height = 10.0;

    pl::GeoHash::GeoHashRadius result{};
    pl::Geo::geohash_cal_area_by_shape(shape, &result);

    EXPECT_NE(0ULL, result.hash.bits);
    EXPECT_TRUE(result.area.contains(shape.center.lng, shape.center.lat));
}

// ============================================================================
// GeoHashBitsComparator tests
// ============================================================================

TEST(GeoHashTest, ComparatorOrdering) {
    pl::GeoHashBitsComparator cmp;

    pl::GeoHash::HashBits a{100, 10};
    pl::GeoHash::HashBits b{200, 10};
    pl::GeoHash::HashBits c{100, 5};

    // Same step, different bits
    EXPECT_TRUE(cmp(a, b));
    EXPECT_FALSE(cmp(b, a));

    // Different step (lower step first)
    EXPECT_TRUE(cmp(c, a));
    EXPECT_FALSE(cmp(a, c));
}

TEST(GeoHashTest, GeoHashBitsSetWorks) {
    pl::GeoHashBitsSet set;
    set.insert({100, 10});
    set.insert({200, 10});
    set.insert({100, 10}); // duplicate
    EXPECT_EQ(2, set.size());
}

// ============================================================================
// Point and Area struct tests
// ============================================================================

TEST(GeoHashTest, PointIsZero) {
    pl::GeoHash::Point zero{0, 0};
    EXPECT_TRUE(zero.is_zero());

    pl::GeoHash::Point nonzero{1.0, 0};
    EXPECT_FALSE(nonzero.is_zero());
}

TEST(GeoHashTest, AreaIsZero) {
    pl::GeoHash::Area zero{pl::GeoHash::Point{0, 0}, pl::GeoHash::Point{1.0, 1.0}};
    EXPECT_TRUE(zero.is_zero()); // sw is zero

    pl::GeoHash::Area nonzero{pl::GeoHash::Point{1.0, 1.0}, pl::GeoHash::Point{2.0, 2.0}};
    EXPECT_FALSE(nonzero.is_zero());
}

TEST(GeoHashTest, AreaCenter) {
    pl::GeoHash::Area area{pl::GeoHash::Point{10.0, 20.0}, pl::GeoHash::Point{30.0, 40.0}};
    auto center = area.center();
    EXPECT_DOUBLE_EQ(20.0, center.lng);
    EXPECT_DOUBLE_EQ(30.0, center.lat);
}

TEST(GeoHashTest, AreaContains) {
    pl::GeoHash::Area area{pl::GeoHash::Point{10.0, 20.0}, pl::GeoHash::Point{30.0, 40.0}};
    EXPECT_TRUE(area.contains(20.0, 30.0));
    EXPECT_TRUE(area.contains(10.0, 20.0)); // boundary
    EXPECT_TRUE(area.contains(30.0, 40.0)); // boundary
    EXPECT_FALSE(area.contains(9.0, 30.0));
    EXPECT_FALSE(area.contains(20.0, 41.0));
}

TEST(GeoHashTest, AreaScale) {
    pl::GeoHash::Area area{pl::GeoHash::Point{10.0, 20.0}, pl::GeoHash::Point{30.0, 50.0}};
    EXPECT_DOUBLE_EQ(20.0, area.lng_scale());
    EXPECT_DOUBLE_EQ(30.0, area.lat_scale());
}

TEST(GeoHashTest, HashBitsIsZero) {
    pl::GeoHash::HashBits zero{0, 0};
    EXPECT_TRUE(zero.is_zero());

    pl::GeoHash::HashBits nonzero1{1, 0};
    EXPECT_FALSE(nonzero1.is_zero());

    pl::GeoHash::HashBits nonzero2{0, 1};
    EXPECT_FALSE(nonzero2.is_zero());
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(GeoHashTest, EncodeAtBoundary) {
    pl::GeoHash::HashBits hash{};
    // Encode at exact boundary values
    bool ok = pl::GeoHash::encode_wgs84(180.0, 85.05112878, 26, &hash);
    ASSERT_TRUE(ok);

    ok = pl::GeoHash::encode_wgs84(-180.0, -85.05112878, 26, &hash);
    ASSERT_TRUE(ok);
}

TEST(GeoHashTest, EncodeMaxStep) {
    pl::GeoHash::HashBits hash{};
    bool ok = pl::GeoHash::encode_wgs84(116.0, 40.0, 32, &hash);
    ASSERT_TRUE(ok);
    EXPECT_EQ(32, hash.step);

    // Decode should work
    pl::GeoHash::Area area{};
    ok = pl::GeoHash::decode_wgs84(hash, &area);
    ASSERT_TRUE(ok);
    // Area should be extremely small
    EXPECT_LT(area.lng_scale(), 1e-6);
    EXPECT_LT(area.lat_scale(), 1e-6);
}

TEST(GeoHashTest, NeighborsAtEquator) {
    // Test neighbors at equator (lat=0) with low step to avoid wraparound
    pl::GeoHash::HashBits hash{};
    pl::GeoHash::encode_wgs84(0.0, 0.0, 5, &hash);

    pl::GeoHash::Neighbors nb{};
    pl::GeoHash::neighbors(&hash, &nb);

    // All should decode properly
    pl::GeoHash::Area n_area{}, s_area{};
    EXPECT_TRUE(pl::GeoHash::decode_wgs84(nb.n, &n_area));
    EXPECT_TRUE(pl::GeoHash::decode_wgs84(nb.s, &s_area));

    // North should be in northern hemisphere, south in southern
    EXPECT_GT(n_area.center().lat, 0.0);
    EXPECT_LT(s_area.center().lat, 0.0);
}

TEST(GeoTest, DistanceAtHighLatitude) {
    // At high latitudes, longitude lines converge
    pl::GeoHash::Point p1{0.0, 80.0};
    pl::GeoHash::Point p2{1.0, 80.0};
    double d_high = pl::Geo::geo_distance(p1, p2);

    pl::GeoHash::Point p3{0.0, 0.0};
    pl::GeoHash::Point p4{1.0, 0.0};
    double d_eq = pl::Geo::geo_distance(p3, p4);

    // 1 degree of longitude at 80° should be much less than at equator
    EXPECT_LT(d_high, d_eq * 0.3);
}
