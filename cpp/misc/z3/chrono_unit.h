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

#include <chrono>

namespace pl::curve {

enum class ChronoUnit { MILLIS, SECONDS, MINUTES, HOURS, DAYS, WEEKS, MONTHS, YEARS };

template <ChronoUnit Unit> struct ChronoDuration;

template <> struct ChronoDuration<ChronoUnit::MILLIS> {
    using type = std::chrono::milliseconds;
};

template <> struct ChronoDuration<ChronoUnit::SECONDS> {
    using type = std::chrono::seconds;
};

template <> struct ChronoDuration<ChronoUnit::MINUTES> {
    using type = std::chrono::minutes;
};

template <> struct ChronoDuration<ChronoUnit::HOURS> {
    using type = std::chrono::hours;
};

template <> struct ChronoDuration<ChronoUnit::DAYS> {
    using type = std::chrono::duration<uint32_t, std::ratio<86400>>;
};

template <> struct ChronoDuration<ChronoUnit::WEEKS> {
    using type = std::chrono::duration<uint32_t, std::ratio<604800>>;
};

template <> struct ChronoDuration<ChronoUnit::MONTHS> {
    using type = std::chrono::duration<uint32_t, std::ratio<31556952ULL / 12>>;
};

template <> struct ChronoDuration<ChronoUnit::YEARS> {
    using type = std::chrono::duration<uint32_t, std::ratio<31556952ULL>>;
};

} // namespace pl::curve
