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

#include <gtest/gtest.h>

std::string hash_tostring(uint64_t bits) {
    constexpr std::string_view alphabets = "0123456789bcdefghjkmnpqrstuvwxyz";
    char buf[12];
    for (int i = 0; i < 11; ++i) {
        int idx = 0;
        if (i != 10) {
            idx = (bits >> (52 - ((i + 1) * 5))) & 0x1f;
        }
        buf[i] = alphabets[idx];
    }
    buf[11] = '\0';
    return buf;
}

TEST(geohash, encode) {
    std::map<std::string, pl::GeoHash::Point> cases = {
        {"wx4ey9n3gm0", pl::GeoHash::Point{116.31, 40.04}}};
    constexpr pl::GeoHash::Rectangle range = {
        pl::GeoHash::Point{-180, -90},
        pl::GeoHash::Point{180, 90},
    };

    for (const auto& [k, v] : cases) {
        pl::GeoHash::HashBits hash;
        pl::GeoHash::encode(range, v.lng, v.lat, 26, &hash);

        // WHY?
        // the hash in redis is: 4068802680076238
        // but 4068802680076235 here
        ::printf("=============%lu\n", hash.bits);

        EXPECT_EQ(k, hash_tostring(hash.bits));
    }
}
