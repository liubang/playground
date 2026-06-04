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
// Created: 2026/06/04 12:01

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pl::sstv2::encode {

class StreamVByte {
public:
    // Batch encode N uint32 values, return total bytes written
    static size_t encode(std::span<const uint32_t> values, std::byte* dst);

    // Batch decode N values from compressed stream
    static void decode(std::span<const std::byte> src, size_t count, uint32_t* dst);

    // Max possible encoded size for N values
    static constexpr size_t max_encoded_size(size_t count) {
        // control bytes: ceil(count/4) + data bytes: count * 4
        return (count + 3) / 4 + count * 4;
    }
};

} // namespace pl::sstv2::encode
