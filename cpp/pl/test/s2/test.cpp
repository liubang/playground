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

#include "absl/strings/str_format.h"
#include "s2/base/commandlineflags.h"
#include "s2/s1angle.h"
#include "s2/s2closest_point_query.h"
#include "s2/s2earth.h"
#include "s2/s2point_index.h"
#include "s2/s2random.h"

int main(int argc, char* argv[]) {
    static constexpr uint64_t count = 10000;

    std::mt19937_64 bitgen;
    // Build an index containing random points anywhere on the Earth.
    S2PointIndex<int> index;
    for (int i = 0; i < count; ++i) {
        index.Add(s2random::Point(bitgen), i);
    }

    // Create a query to search within the given radius of a target point.
    S2ClosestPointQuery<int> query(&index);
    query.mutable_options()->set_max_distance(S1Angle::Radians(S2Earth::KmToRadians(100)));

    // Repeatedly choose a random target point, and count how many index points
    // are within the given radius of that point.
    int64_t num_found = 0;
    for (int i = 0; i < count; ++i) {
        S2ClosestPointQuery<int>::PointTarget target(s2random::Point(bitgen));
        num_found += query.FindClosestPoints(&target).size();
    }

    absl::PrintF("Found %d points in %d queries\n", num_found, count);

    return 0;
}
