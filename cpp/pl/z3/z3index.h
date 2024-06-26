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
#include "z3sfc.h"

#include <memory>
#include <string_view>

namespace pl::curve {

class Z3IndexKey {
public:
    Z3IndexKey(uint16_t bin, uint64_t z) : bin_(bin), z_(z) {}

    [[nodiscard]] uint16_t bin() const { return bin_; }

    [[nodiscard]] uint64_t z() const { return z_; }

    [[nodiscard]] bool compare(const Z3IndexKey& other) const {
        if (bin_ != other.bin_) {
            return bin_ < other.bin();
        }
        return z_ < other.z();
    }

private:
    uint16_t bin_; // date epoch
    uint64_t z_;   // z3 value within the epoch
};

class Z3IndexValues {
public:
private:
    std::vector<Box> spatial_bounds_;
};

template <TimePeriod period> class Z3IndexKeySpace {
public:
    Z3IndexKeySpace() { sfc_ = std::make_unique<Z3SFC<period>>(); }

    /**
     * +----------+------------+-------------+------------+
     * | shard id | epoch time | z3(x, y, t) | feathre id |
     * +----------+------------+-------------+------------+
     * |<-- 1B -->|<--  2B  -->|<--   8B  -->|<--  ... -->|
     */
    std::string to_idx_key(uint8_t shard,
                           std::string_view id,
                           double x,
                           double y,
                           const std::chrono::time_point<std::chrono::system_clock>& time) {
        std::string rowkey;
        rowkey.reserve(11 + id.size());
        auto binned_time = BinnedTime<period>::of(time);
        auto z = sfc_->index(x, y, binned_time.offset());

        rowkey.append(reinterpret_cast<const char*>(&shard), sizeof(shard)); // 1 byte
        rowkey.append(reinterpret_cast<const char*>(&binned_time.bin()),
                      sizeof(binned_time.bin()));                    // 2 byte
        rowkey.append(reinterpret_cast<const char*>(&z), sizeof(z)); // 8 byte
        rowkey.append(id);
        return rowkey;
    }

    // TODO
    void get_ranges() {}

private:
    std::unique_ptr<Z3SFC<period>> sfc_;
};

} // namespace pl::curve
