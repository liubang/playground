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
// Created: 2026/06/04 15:23

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"

namespace pl::sstv2::file {

struct SSTableBuilderOptions {
    size_t block_size = 65536;
    uint8_t compression = 0; // 0=none
    size_t bloom_bits_per_key = 10;
    size_t value_size_threshold = 1024;
    std::string value_file_path; // empty = no KV separation
};

class SSTableBuilder {
public:
    using Options = SSTableBuilderOptions;

    explicit SSTableBuilder(std::string_view output_path, Options opts = {});

    // Add a raw key-value pair (simplified interface)
    absl::Status add(std::string_view key, std::string_view value);

    // Finish building the SST file
    absl::Status finish();

    // Abort (file left for GC)
    void abort();

    uint64_t total_rows() const;
    uint64_t data_size() const;

private:
    Options opts_;
    std::string output_path_;
    std::string buffer_; // accumulates the full file content
    uint64_t total_rows_ = 0;
    uint64_t data_size_ = 0;
    bool finished_ = false;
    bool aborted_ = false;
};

} // namespace pl::sstv2::file
