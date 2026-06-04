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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "cpp/pl/sstv2/pattern/pattern_encoder.h"

namespace pl::sstv2::pattern {

struct Selection {
    PatternId pattern_id;
    std::unique_ptr<PatternEncoder> encoder;
    size_t estimated_size;
};

class PatternSelector {
public:
    static Selection select(std::span<const uint64_t> values);
};

} // namespace pl::sstv2::pattern
