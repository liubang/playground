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
// Created: 2026/06/06 14:28

#pragma once

#include <crc32c/crc32c.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pl::sstv2::codec {

[[nodiscard]] inline uint64_t crc32c_u64(std::string_view bytes) noexcept {
    return static_cast<uint64_t>(::crc32c::Crc32c(bytes));
}

[[nodiscard]] inline uint64_t crc32c_u64_with_zeroed_range(std::string_view bytes,
                                                           size_t offset,
                                                           size_t length) {
    std::string copy(bytes);
    copy.replace(offset, length, length, '\0');
    return crc32c_u64(copy);
}

} // namespace pl::sstv2::codec
