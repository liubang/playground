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

#include "chrono_unit.h"

#include <chrono>
#include <cstdint>

namespace pl::curve {

enum class TimePeriod {
    Day,
    Week,
    Month,
    Year,
};

template <TimePeriod period> class BinnedTime {
public:
    static constexpr uint64_t max_offset() {
        if constexpr (period == TimePeriod::Week) {
            return 604800ULL;
        } else if constexpr (period == TimePeriod::Month) {
            return 2678400ULL;
        } else if constexpr (period == TimePeriod::Year) {
            return 527050ULL;
        } else {
            return 86400000ULL;
        }
    }

    static BinnedTime<period> of(
        const std::chrono::time_point<std::chrono::system_clock>& time_point) {
        if constexpr (period == TimePeriod::Week) {
            uint16_t weeks = to_epochs<ChronoUnit::WEEKS>(time_point);
            std::chrono::time_point<std::chrono::system_clock> epoch;
            auto time = epoch + ChronoDuration<ChronoUnit::WEEKS>::type(weeks);
            uint64_t offset = to_epochs<ChronoUnit::SECONDS>(time_point) =
                to_epochs<ChronoUnit::SECONDS>(time);
            return BinnedTime<period>{weeks, offset};
        } else if constexpr (period == TimePeriod::Month) {
            uint16_t months = to_epochs<ChronoUnit::MONTHS>(time_point);
            std::chrono::time_point<std::chrono::system_clock> epoch;
            auto time = epoch + ChronoDuration<ChronoUnit::MONTHS>::type(months);
            uint64_t offset = to_epochs<ChronoUnit::SECONDS>(time_point) =
                to_epochs<ChronoUnit::SECONDS>(time);
            return BinnedTime<period>{months, offset};
        } else if constexpr (period == TimePeriod::Year) {
            uint16_t years = to_epochs<ChronoUnit::YEARS>(time_point);
            std::chrono::time_point<std::chrono::system_clock> epoch;
            auto time = epoch + ChronoDuration<ChronoUnit::YEARS>::type(years);
            uint64_t offset = to_epochs<ChronoUnit::SECONDS>(time_point) =
                to_epochs<ChronoUnit::SECONDS>(time);
            return BinnedTime<period>{years, offset};
        } else {
            uint16_t days = to_epochs<ChronoUnit::DAYS>(time_point);
            std::chrono::time_point<std::chrono::system_clock> epoch;
            auto time = epoch + ChronoDuration<ChronoUnit::DAYS>::type(days);
            uint64_t offset = to_epochs<ChronoUnit::MILLIS>(time_point) =
                to_epochs<ChronoUnit::MILLIS>(time);
            return BinnedTime<period>{days, offset};
        }
    }

    /**
     * @brief number of time periods from the epoch
     *
     * @return uint16_t
     */
    [[nodiscard]] uint16_t bin() const { return bin_; }

    /**
     * @brief precise offset into the specific time period
     *
     * @return uint64_t
     */
    [[nodiscard]] uint64_t offset() const { return offset_; }

private:
    template <ChronoUnit Unit>
    static uint64_t to_epochs(
        const std::chrono::time_point<std::chrono::system_clock>& time_point) {
        return std::chrono::duration_cast<ChronoDuration<Unit>::type>(time_point.time_since_epoch())
            .count();
    }

private:
    uint16_t bin_;
    uint64_t offset_;
};

} // namespace pl::curve
