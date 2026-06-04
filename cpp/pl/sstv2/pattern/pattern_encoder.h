// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/06/04 13:06

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "absl/status/status.h"

namespace pl::sstv2::pattern {

enum class PatternId : uint8_t {
    kNone = 0,
    kStreamVByte = 1,
    kPFor = 2,
    kDictionary = 3,
    kDeltaIncrement = 4,
    kDeltaDecrement = 5,
    kConstant = 6,
};

class PatternEncoder {
public:
    virtual ~PatternEncoder() = default;
    virtual absl::Status encode(std::span<const uint64_t> values, std::string& output) = 0;
    virtual PatternId pattern_id() const = 0;
};

} // namespace pl::sstv2::pattern
