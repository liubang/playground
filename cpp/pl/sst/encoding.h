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

#include <cstring>
#include <string>
#include <type_traits>

namespace pl {

/**
 * @brief
 *
 * @tparam T
 * @param dst
 * @param value
 */
template <typename T> void encodeInt(std::string* dst, T value) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "T must be an integral type but not bool");
    dst->reserve(dst->size() + sizeof(T));
    dst->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

/**
 * @brief
 *
 * @tparam T
 * @param input
 * @return
 */
template <typename T> T decodeInt(const char* input) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "T must be an integral type but not bool");
    T value;
    memcpy(&value, input, sizeof(T));
    return value;
}

} // namespace pl
