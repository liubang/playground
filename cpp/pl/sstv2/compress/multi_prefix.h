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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace pl::sstv2::compress {

struct MultiPrefixConfig {
    size_t max_rounds = 4;
    size_t min_prefix_len = 4;
    double min_compression_ratio = 0.1; // stop if single round saves < 10%
};

struct MultiPrefixResult {
    std::string compressed_data;
    std::vector<std::string> prefix_directory;
    size_t num_rounds = 0;
};

class MultiPrefixCompressor {
public:
    using Config = MultiPrefixConfig;
    using CompressResult = MultiPrefixResult;

    explicit MultiPrefixCompressor(Config config = Config{});

    // Compress a batch of sorted strings
    absl::StatusOr<CompressResult> compress(std::span<const std::string_view> sorted_strings);

    // Decompress one string by index
    static absl::StatusOr<std::string> decompress_one(
        std::span<const std::byte> compressed_data,
        const std::vector<std::string>& prefix_directory,
        size_t num_strings,
        size_t idx);

    // Decompress all strings
    static absl::StatusOr<std::vector<std::string>> decompress_all(
        std::span<const std::byte> compressed_data,
        const std::vector<std::string>& prefix_directory,
        size_t num_strings);

private:
    Config config_;
};

} // namespace pl::sstv2::compress
