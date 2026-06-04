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
#include <span>
#include <string>

#include "absl/status/statusor.h"

namespace pl::sstv2::compress {

enum class CompressionType : uint8_t {
    kNone = 0,
    kSnappy = 1,
    kZstd = 2,
};

class BlockCompressor {
public:
    static absl::StatusOr<std::string> compress(CompressionType type,
                                                std::span<const std::byte> input);
    static absl::StatusOr<std::string> decompress(CompressionType type,
                                                  std::span<const std::byte> compressed,
                                                  size_t uncompressed_size);
    static size_t max_compressed_size(CompressionType type, size_t input_size);
};

} // namespace pl::sstv2::compress
