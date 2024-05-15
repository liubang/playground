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

#include <cassert>
#include <cmath>
#include <cstdint>

namespace pl::curve {

class NormalizedDimension {
public:
    virtual double min() = 0;

    virtual double max() = 0;

    virtual uint64_t max_index() = 0;

    virtual uint64_t normalize(double x) = 0;

    virtual double denormalize(uint64_t x) = 0;
};

class BitNormalizedDimension : public NormalizedDimension {
public:
    BitNormalizedDimension(double min, double max, uint32_t precision) {
        assert(precision > 0 && precision < 32);
        min_ = min;
        max_ = max;
        bins_ = 1ULL << precision;
        normalizer_ = (double)bins_ / (max - min);
        denormalizer_ = (max - min) / (double)bins_;
        max_index_ = (int64_t)(bins_ - 1ULL);
    }

    virtual ~BitNormalizedDimension() = default;

    double min() override { return min_; }

    double max() override { return max_; }

    uint64_t max_index() override { return max_index_; }

    uint64_t normalize(double x) override {
        return x >= max_ ? max_index_ : (uint64_t)std::floor((x - min_) * normalizer_);
    }

    double denormalize(uint64_t x) override {
        return x >= max_index_ ? min_ + ((double)max_index_ + 0.5) * denormalizer_
                               : min_ + ((double)x + 0.5) * denormalizer_;
    }

private:
    double min_;
    double max_;
    uint64_t bins_;
    double normalizer_;
    double denormalizer_;
    uint64_t max_index_;
};

class NormalizedLat : public BitNormalizedDimension {
public:
    NormalizedLat(uint32_t precision) : BitNormalizedDimension(-90.0, 90.0, precision) {}
    ~NormalizedLat() override = default;
};

class NormalizedLon : public BitNormalizedDimension {
public:
    NormalizedLon(uint32_t precision) : BitNormalizedDimension(-180.0, 180.0, precision) {}
    ~NormalizedLon() override = default;
};

class NormalizedTime : public BitNormalizedDimension {
public:
    NormalizedTime(uint32_t precision, double max) : BitNormalizedDimension(0.0, max, precision) {}
    ~NormalizedTime() override = default;
};

} // namespace pl::curve
