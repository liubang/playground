// Copyright (c) 2025 The Authors. All rights reserved.
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

#include <cstdint>
#include <string_view>

namespace pl::test {

using status_code_t = uint16_t;

#define RAW_STATUS(name, value)                     \
    namespace StatusCode {                          \
    inline constexpr status_code_t k##name = value; \
    }

#define STATUS(ns, name, value)                         \
    namespace ns##Code {                                \
        inline constexpr status_code_t k##name = value; \
    }

#include "status_code_detail.h"

#undef STATUS
#undef RAW_STATUS

enum class StatusCodeType {
    Invalid = -1,
    Common = 0,
};

namespace StatusCode {
std::string_view toString(status_code_t code);

StatusCodeType typeOf(status_code_t code);
} // namespace StatusCode

} // namespace pl::test
