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
// Created: 2025/07/01 00:45

#pragma once

#include <cstdint>
#include <string_view>

namespace pl::minidfs {

enum class CompressionType : uint8_t {
    kNone = 0,
    kSnappy = 1,
    kZstd = 2,
};

/// Return the human-readable name of a compression type.
constexpr std::string_view compression_type_name(CompressionType type) {
    switch (type) {
        case CompressionType::kNone:
            return "none";
        case CompressionType::kSnappy:
            return "snappy";
        case CompressionType::kZstd:
            return "zstd";
    }
    return "unknown";
}

} // namespace pl::minidfs
