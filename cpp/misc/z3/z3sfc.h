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

#include "binned_time.h"
#include "dimension.h"
#include "z3.h"

#include <memory>

namespace pl::curve {

template <TimePeriod period> class Z3SFC {
public:
    Z3SFC(uint32_t precision) {
        // precision (bits) per dimendion must be in [1, 21];
        assert(precision > 0 && precision < 22);
        lon_ = std::make_unique<NormalizedLon>(precision);
        lat_ = std::make_unique<NormalizedLat>(precision);
        time_ = std::make_unique<NormalizedTime>(precision, BinnedTime<period>::max_offset());
    }

    Z3SFC() : Z3SFC<period>(21) {}

    uint64_t index(double x, double y, uint64_t t) {
        return Z3(lon_->normalize(x), lat_->normalize(y), time_->normalize(t)).val();
    }

    std::tuple<double, double, uint64_t> invert(uint64_t z) { return Z3(z).decode(); }

private:
    std::unique_ptr<NormalizedDimension> lon_;
    std::unique_ptr<NormalizedDimension> lat_;
    std::unique_ptr<NormalizedDimension> time_;
};

} // namespace pl::curve
